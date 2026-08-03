/*
 * cocoa_window.m
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */


#include "state.h"
#include "cleanup.h"
#include "cocoa_window.h"
#include <Availability.h>
#include <Carbon/Carbon.h>
#include <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/IOKitLib.h>
#include <UserNotifications/UserNotifications.h>
#import <AudioToolbox/AudioServices.h>

#include <AvailabilityMacros.h>
// Needed for _NSGetProgname
#include <crt_externs.h>
#include <objc/runtime.h>

static inline void cleanup_cfrelease(void *__p) { CFTypeRef *tp = (CFTypeRef *)__p; CFTypeRef cf = *tp; if (cf) { CFRelease(cf); } }
#define RAII_CoreFoundation(type, name, initializer) __attribute__((cleanup(cleanup_cfrelease))) type name = initializer

#if (MAC_OS_X_VERSION_MAX_ALLOWED < 101300)
#define NSControlStateValueOn NSOnState
#define NSControlStateValueOff NSOffState
#define NSControlStateValueMixed NSMixedState
#endif
#if (MAC_OS_X_VERSION_MAX_ALLOWED < 101200)
#define NSWindowStyleMaskResizable NSResizableWindowMask
#define NSEventModifierFlagOption NSAlternateKeyMask
#define NSEventModifierFlagCommand NSCommandKeyMask
#define NSEventModifierFlagControl NSControlKeyMask
#endif
#if (MAC_OS_X_VERSION_MAX_ALLOWED < 110000)
#define UNNotificationPresentationOptionList (1 << 3)
#define UNNotificationPresentationOptionBanner (1 << 4)
#endif

typedef int CGSConnectionID;
typedef int CGSWindowID;
typedef int CGSWorkspaceID;
typedef enum _CGSSpaceSelector {
    kCGSSpaceCurrent = 5,
    kCGSSpaceAll = 7
} CGSSpaceSelector;
extern CGSConnectionID _CGSDefaultConnection(void);
CFArrayRef CGSCopySpacesForWindows(CGSConnectionID Connection, CGSSpaceSelector Type, CFArrayRef Windows);

static NSMenuItem* title_menu = NULL;
static NSMenuItem* secure_input_title_menu = NULL;
static bool application_has_finished_launching = false;


static NSString*
find_app_name(void) {
    size_t i;
    NSDictionary* infoDictionary = [[NSBundle mainBundle] infoDictionary];

    // Keys to search for as potential application names
    NSString* name_keys[] =
    {
        @"CFBundleDisplayName",
        @"CFBundleName",
        @"CFBundleExecutable",
    };

    for (i = 0;  i < sizeof(name_keys) / sizeof(name_keys[0]);  i++)
    {
        id name = infoDictionary[name_keys[i]];
        if (name &&
            [name isKindOfClass:[NSString class]] &&
            ![name isEqualToString:@""])
        {
            return name;
        }
    }

    char** progname = _NSGetProgname();
    if (progname && *progname)
        return @(*progname);

    // Really shouldn't get here
    return @"kitty";
}

#define debug_key(...) if (OPT(debug_keyboard)) { fprintf(stderr, __VA_ARGS__); fflush(stderr); }

// SecureKeyboardEntryController {{{
@interface SecureKeyboardEntryController : NSObject

@property (nonatomic, readonly) BOOL isDesired;
@property (nonatomic, readonly, getter=isEnabled) BOOL enabled;

+ (instancetype)sharedInstance;

- (void)toggle;
- (void)update;

@end

@implementation SecureKeyboardEntryController {
    int _count;
    BOOL _desired;
}

+ (instancetype)sharedInstance {
    static id instance;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _desired = false;

        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(applicationDidResignActive:)
                                                     name:NSApplicationDidResignActiveNotification
                                                   object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(applicationDidBecomeActive:)
                                                     name:NSApplicationDidBecomeActiveNotification
                                                   object:nil];
        if ([NSApp isActive]) {
            [self update];
        }
    }
    return self;
}

#pragma mark - API

- (void)toggle {
    // Set _desired to the opposite of the current state.
    _desired = !_desired;
    debug_key("SecureKeyboardEntry: toggle called. Setting desired to %d ", _desired);

    // Try to set the system's state of secure input to the desired state.
    [self update];
}

- (BOOL)isEnabled {
    return !!IsSecureEventInputEnabled();
}

- (BOOL)isDesired {
    return _desired;
}

#pragma mark - Notifications

- (void)applicationDidResignActive:(NSNotification *)notification {
    (void)notification;
    if (_count > 0) {
        debug_key("SecureKeyboardEntry: Application resigning active.");
        [self update];
    }
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    if (self.isDesired) {
        debug_key("SecureKeyboardEntry: Application became active.");
        [self update];
    }
}

#pragma mark - Private

- (BOOL)allowed {
    return [NSApp isActive];
}

- (void)update {
    debug_key("Update secure keyboard entry. desired=%d active=%d\n",
         (int)self.isDesired, (int)[NSApp isActive]);
    const BOOL secure = self.isDesired && [self allowed];

    if (secure && _count > 0) {
        debug_key("Want to turn on secure input but it's already on\n");
        return;
    }

    if (!secure && _count == 0) {
        debug_key("Want to turn off secure input but it's already off\n");
        return;
    }

    debug_key("Before: IsSecureEventInputEnabled returns %d ", (int)self.isEnabled);
    if (secure) {
        OSErr err = EnableSecureEventInput();
        debug_key("EnableSecureEventInput err=%d ", (int)err);
        if (err) {
            debug_key("EnableSecureEventInput failed with error %d ", (int)err);
        } else {
            _count += 1;
        }
    } else {
        OSErr err = DisableSecureEventInput();
        debug_key("DisableSecureEventInput err=%d ", (int)err);
        if (err) {
            debug_key("DisableSecureEventInput failed with error %d ", (int)err);
        } else {
            _count -= 1;
        }
    }
    debug_key("After: IsSecureEventInputEnabled returns %d\n", (int)self.isEnabled);
}

@end
// }}}

static void
update_secure_input_menu_bar_indicator(BOOL enabled) {
    if (enabled) {
        if (secure_input_title_menu == NULL) {
            NSMenu *bar = [NSApp mainMenu];
            secure_input_title_menu = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
            NSMenu *m = [[NSMenu alloc] initWithTitle:@"[Secure input]"];
            [secure_input_title_menu setSubmenu:m];
            [m release];
        }
    } else {
        if (secure_input_title_menu != NULL) {
            NSMenu *bar = [NSApp mainMenu];
            [bar removeItem:secure_input_title_menu];
            secure_input_title_menu = NULL;
        }
    }
}

@interface UserMenuItem : NSMenuItem
@property (nonatomic) size_t action_index;
@end

@implementation UserMenuItem {
}
@end



@interface GlobalMenuTarget : NSObject
+ (GlobalMenuTarget *) shared_instance;
@end

#define PENDING(selector, which) - (void)selector:(id)sender { (void)sender; set_cocoa_pending_action(which, NULL); }

@implementation GlobalMenuTarget

- (void)user_menu_action:(id)sender {
    UserMenuItem *m = sender;
    if (m.action_index < OPT(global_menu).count && OPT(global_menu.entries)) {
        set_cocoa_pending_action(USER_MENU_ACTION, OPT(global_menu).entries[m.action_index].definition);
    }
}

PENDING(edit_config_file, PREFERENCES_WINDOW)
PENDING(new_os_window, NEW_OS_WINDOW)
PENDING(detach_tab, DETACH_TAB)
PENDING(close_os_window, CLOSE_OS_WINDOW)
PENDING(close_tab, CLOSE_TAB)
PENDING(new_tab, NEW_TAB)
PENDING(next_tab, NEXT_TAB)
PENDING(previous_tab, PREVIOUS_TAB)
PENDING(new_window, NEW_WINDOW)
PENDING(close_window, CLOSE_WINDOW)
PENDING(reset_terminal, RESET_TERMINAL)
PENDING(clear_terminal_and_scrollback, CLEAR_TERMINAL_AND_SCROLLBACK)
PENDING(clear_scrollback, CLEAR_SCROLLBACK)
PENDING(clear_screen, CLEAR_SCREEN)
PENDING(clear_last_command, CLEAR_LAST_COMMAND)
PENDING(reload_config, RELOAD_CONFIG)
PENDING(toggle_macos_secure_keyboard_entry, TOGGLE_MACOS_SECURE_KEYBOARD_ENTRY)
PENDING(macos_cycle_through_os_windows, MACOS_CYCLE_THROUGH_OS_WINDOWS)
PENDING(macos_cycle_through_os_windows_backwards, MACOS_CYCLE_THROUGH_OS_WINDOWS_BACKWARDS)
PENDING(search_scrollback, SEARCH_SCROLLBACK)
PENDING(toggle_fullscreen, TOGGLE_FULLSCREEN)
PENDING(open_kitty_website, OPEN_KITTY_WEBSITE)
PENDING(hide_macos_app, HIDE)
PENDING(hide_macos_other_apps, HIDE_OTHERS)
PENDING(minimize_macos_window, MINIMIZE)
PENDING(quit, QUIT)
PENDING(paste_from_clipboard, PASTE_FROM_CLIPBOARD)
PENDING(copy_or_noop, COPY_OR_NOOP)

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(toggle_macos_secure_keyboard_entry:)) {
        item.state = [SecureKeyboardEntryController sharedInstance].isDesired ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(toggle_fullscreen:)) {
        item.title = ([NSApp currentSystemPresentationOptions] & NSApplicationPresentationFullScreen) ? @"Exit Full Screen" : @"Enter Full Screen";
        if (![NSApp keyWindow]) return NO;
    } else if (item.action == @selector(minimize_macos_window:)) {
        NSWindow *window = [NSApp keyWindow];
        if (!window || window.miniaturized || [NSApp currentSystemPresentationOptions] & NSApplicationPresentationFullScreen) return NO;
    } else if (item.action == @selector(close_os_window:) ||
        item.action == @selector(close_tab:) ||
        item.action == @selector(close_window:) ||
        item.action == @selector(reset_terminal:) ||
        item.action == @selector(clear_terminal_and_scrollback:) ||
        item.action == @selector(clear_last_command:) ||
        item.action == @selector(clear_scrollback:) ||
        item.action == @selector(clear_screen:) ||
        item.action == @selector(previous_tab:) ||
        item.action == @selector(next_tab:) ||
        item.action == @selector(detach_tab:))
    {
        if (![NSApp keyWindow]) return NO;
    } else if (item.action == @selector(paste_from_clipboard:)) {
        if (![NSApp keyWindow]) return NO;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        if (![pb stringForType:NSPasteboardTypeString]) return NO;
    } else if (item.action == @selector(copy_or_noop:)) {
        if (![NSApp keyWindow]) return NO;
        OSWindow *osw = current_os_window();
        if (osw && osw->num_tabs > osw->active_tab) {
            Tab *tab = osw->tabs + osw->active_tab;
            if (tab->num_windows > tab->active_window) {
                Screen *screen = tab->windows[tab->active_window].render_data.screen;
                if (screen && screen_has_selection(screen)) return YES;
            }
        }
        return NO;
    }
    return YES;
}

#undef PENDING

+ (GlobalMenuTarget *) shared_instance
{
    static GlobalMenuTarget *sharedGlobalMenuTarget = nil;
    @synchronized(self)
    {
        if (!sharedGlobalMenuTarget) {
            sharedGlobalMenuTarget = [[GlobalMenuTarget alloc] init];
            SecureKeyboardEntryController *k = [SecureKeyboardEntryController sharedInstance];
            if (!k.isDesired && [[NSUserDefaults standardUserDefaults] boolForKey:@"SecureKeyboardEntry"]) [k toggle];
            update_secure_input_menu_bar_indicator(k.isDesired);
        }
        return sharedGlobalMenuTarget;
    }
}

@end

typedef struct {
    char key[32];
    NSEventModifierFlags mods;
} GlobalShortcut;
typedef struct {
    GlobalShortcut new_os_window, close_os_window, close_tab, edit_config_file, reload_config;
    GlobalShortcut previous_tab, next_tab, new_tab, new_window, close_window, reset_terminal;
    GlobalShortcut clear_terminal_and_scrollback, clear_screen, clear_scrollback, clear_last_command;
    GlobalShortcut toggle_macos_secure_keyboard_entry, toggle_fullscreen, open_kitty_website;
    GlobalShortcut hide_macos_app, hide_macos_other_apps, minimize_macos_window, quit, search_scrollback;
    GlobalShortcut macos_cycle_through_os_windows, macos_cycle_through_os_windows_backwards;
    GlobalShortcut paste_from_clipboard, copy_or_noop;
} GlobalShortcuts;
static GlobalShortcuts global_shortcuts;

static PyObject*
cocoa_set_global_shortcut(PyObject *self UNUSED, PyObject *args) {
    int mods;
    unsigned int key;
    const char *name;
    if (!PyArg_ParseTuple(args, "siI", &name, &mods, &key)) return NULL;
    GlobalShortcut *gs = NULL;
#define Q(x) if (strcmp(name, #x) == 0) gs = &global_shortcuts.x
    Q(new_os_window); else Q(close_os_window); else Q(close_tab); else Q(edit_config_file);
    else Q(new_tab); else Q(next_tab); else Q(previous_tab);
    else Q(new_window); else Q(close_window); else Q(reset_terminal);
    else Q(clear_terminal_and_scrollback); else Q(clear_scrollback); else Q(clear_screen); else Q(clear_last_command);
    else Q(reload_config); else Q(toggle_macos_secure_keyboard_entry); else Q(toggle_fullscreen);
    else Q(open_kitty_website); else Q(hide_macos_app); else Q(hide_macos_other_apps);
    else Q(minimize_macos_window); else Q(quit); else Q(search_scrollback);
    else Q(macos_cycle_through_os_windows); else Q(macos_cycle_through_os_windows_backwards);
    else Q(paste_from_clipboard); else Q(copy_or_noop);
#undef Q
    if (gs == NULL) { PyErr_SetString(PyExc_KeyError, "Unknown shortcut name"); return NULL; }
    int cocoa_mods;
    get_cocoa_key_equivalent(key, mods, gs->key, 32, &cocoa_mods);
    gs->mods = cocoa_mods;
    if (gs->key[0]) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// Implementation of applicationDockMenu: for the app delegate
static NSMenu *dockMenu = nil;
static NSMenu *
get_dock_menu(id self UNUSED, SEL _cmd UNUSED, NSApplication *sender UNUSED) {
    if (!dockMenu) {
        GlobalMenuTarget *global_menu_target = [GlobalMenuTarget shared_instance];
        dockMenu = [[NSMenu alloc] init];
        [[dockMenu addItemWithTitle:@"New OS Window"
                             action:@selector(new_os_window:)
                      keyEquivalent:@""]
                          setTarget:global_menu_target];
    }
    return dockMenu;
}

static PyObject *notification_activated_callback = NULL;

static PyObject*
set_notification_activated_callback(PyObject *self UNUSED, PyObject *callback) {
    Py_CLEAR(notification_activated_callback);
    if (callback != Py_None) notification_activated_callback = Py_NewRef(callback);
    Py_RETURN_NONE;
}

static void
do_notification_callback(const char *identifier, const char *event, const char *action_identifer) {
    if (notification_activated_callback) {
        PyObject *ret = PyObject_CallFunction(notification_activated_callback, "sss", event,
                identifier ? identifier : "", action_identifer ? action_identifer : "");
        if (ret) Py_DECREF(ret);
        else PyErr_Print();
    }
}


@interface NotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation NotificationDelegate
    - (void)userNotificationCenter:(UNUserNotificationCenter *)center
            willPresentNotification:(UNNotification *)notification
            withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
        (void)(center); (void)notification;
        UNNotificationPresentationOptions options = UNNotificationPresentationOptionSound;
        if (@available(macOS 11.0, *)) options |= UNNotificationPresentationOptionList | UNNotificationPresentationOptionBanner;
        else options |= (1 << 2); // UNNotificationPresentationOptionAlert avoid deprecated warning
        completionHandler(options);
    }

    - (void)userNotificationCenter:(UNUserNotificationCenter *)center
            didReceiveNotificationResponse:(UNNotificationResponse *)response
            withCompletionHandler:(void (^)(void))completionHandler {
        (void)(center);
        char *identifier = strdup(response.notification.request.identifier.UTF8String);
        char *action_identifier = strdup(response.actionIdentifier.UTF8String);
        const char *event = "button";
        if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
            event = "activated";
        } else if ([response.actionIdentifier isEqualToString:UNNotificationDismissActionIdentifier]) {
            // Crapple never actually sends this event on macOS
            event = "closed";
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            do_notification_callback(identifier, event, action_identifier);
            free(identifier); free(action_identifier);
        });
        completionHandler();
    }
@end

static UNUserNotificationCenter*
get_notification_center_safely(void) {
    NSBundle *b = [NSBundle mainBundle];
    // when bundleIdentifier is nil currentNotificationCenter crashes instead
    // of returning nil. Apple...purveyor of shiny TOYS
    if (!b || !b.bundleIdentifier) return nil;
    UNUserNotificationCenter *center = nil;
    @try {
        center = [UNUserNotificationCenter currentNotificationCenter];
    } @catch (NSException *e) {
        log_error("Failed to get current UNUserNotificationCenter object with error: %s (%s)",
                            [[e name] UTF8String], [[e reason] UTF8String]);
    }
    return center;
}

static bool
ident_in_list_of_notifications(NSString *ident, NSArray<UNNotification*> *list) {
    for (UNNotification *n in list) {
        if ([[[n request] identifier] isEqualToString:ident]) return true;
    }
    return false;
}

void
cocoa_report_live_notifications(const char* ident) {
    do_notification_callback(ident, "live", ident ? ident : "");
}

static bool
remove_delivered_notification(const char *identifier) {
    UNUserNotificationCenter *center = get_notification_center_safely();
    if (!center) return false;
    char *ident = strdup(identifier);
    [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification *> * notifications) {
        if (ident_in_list_of_notifications(@(ident), notifications)) {
            [center removeDeliveredNotificationsWithIdentifiers:@[ @(ident) ]];
        }
        free(ident);
    }];
    return true;
}

static bool
live_delivered_notifications(void) {
    UNUserNotificationCenter *center = get_notification_center_safely();
    if (!center) return false;
    [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification *> * notifications) {
        @autoreleasepool {
            NSMutableString *buffer = [NSMutableString stringWithCapacity:1024];  // autoreleased
            for (UNNotification *n in notifications) [buffer appendFormat:@"%@,", [[n request] identifier]];
            const char *val = [buffer UTF8String];
            set_cocoa_pending_action(COCOA_NOTIFICATION_UNTRACKED, val ? val : "");
        }
    }];
    return true;
}

static void
schedule_notification(const char *appname, const char *identifier, const char *title, const char *body, const char *image_path, int urgency, const char *category_id, bool muted) {@autoreleasepool {
    UNUserNotificationCenter *center = get_notification_center_safely();
    if (!center) return;
    // Configure the notification's payload.
    UNMutableNotificationContent *content = [[[UNMutableNotificationContent alloc] init] autorelease];
    if (title) content.title = @(title);
    if (body) content.body = @(body);
    if (appname) content.threadIdentifier = @(appname);
    if (category_id) content.categoryIdentifier = @(category_id);
    if (!muted) content.sound = [UNNotificationSound defaultSound];
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 120000
    switch (urgency) {
        case 0:
            content.interruptionLevel = UNNotificationInterruptionLevelPassive;
        case 2:
            content.interruptionLevel = UNNotificationInterruptionLevelCritical;
        default:
            content.interruptionLevel = UNNotificationInterruptionLevelActive;
    }
#else
    if ([content respondsToSelector:@selector(interruptionLevel)]) {
        NSUInteger level = 1;
        if (urgency == 0) level = 0; else if (urgency == 2) level = 3;
        [content setValue:@(level) forKey:@"interruptionLevel"];
    }
#endif
    if (image_path) {
        @try {
            NSError *error;
            NSURL *image_url = [NSURL fileURLWithFileSystemRepresentation:image_path isDirectory:NO relativeToURL:nil];  // autoreleased
            UNNotificationAttachment *attachment = [UNNotificationAttachment attachmentWithIdentifier:@"image" URL:image_url options:nil error:&error];  // autoreleased
            if (attachment) { content.attachments = @[ attachment ]; }
            else NSLog(@"Error attaching image %@ to notification: %@", @(image_path), error.localizedDescription);
        } @catch(NSException *exc) {
            NSLog(@"Creating image attachment %@ for notification failed with error: %@", @(image_path), exc.reason);
        }
    }

    // Deliver the notification
    static unsigned long counter = 1;
    UNNotificationRequest* request = [
        UNNotificationRequest requestWithIdentifier:(identifier ? @(identifier) : [NSString stringWithFormat:@"Id_%lu", counter++])
        content:content trigger:nil];
    char *duped_ident = strdup(identifier ? identifier : "");
    [center addNotificationRequest:request withCompletionHandler:^(NSError * _Nullable error) {
        if (error != nil) log_error("Failed to show notification: %s", [[error localizedDescription] UTF8String]);
        bool ok = error == nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            do_notification_callback(duped_ident, ok ? "created" : "creation_failed", "");
            free(duped_ident);
        });
    }];
}}


typedef struct {
    char *identifier, *title, *body, *appname, *image_path, *category_id;
    int urgency; bool muted;
} QueuedNotification;

typedef struct {
    QueuedNotification *notifications;
    size_t count, capacity;
} NotificationQueue;
static NotificationQueue notification_queue = {0};

static void
queue_notification(const char *appname, const char *identifier, const char *title, const char* body, const char *image_path, int urgency, const char *category_id, bool muted) {
    ensure_space_for((&notification_queue), notifications, QueuedNotification, notification_queue.count + 16, capacity, 16, true);
    QueuedNotification *n = notification_queue.notifications + notification_queue.count++;
#define d(x) n->x = (x && x[0]) ? strdup(x) : NULL;
    d(appname); d(identifier); d(title); d(body); d(image_path); d(category_id);
#undef d
    n->urgency = urgency; n->muted = muted;
}

static void
drain_pending_notifications(BOOL granted) {
    if (granted) {
        for (size_t i = 0; i < notification_queue.count; i++) {
            QueuedNotification *n = notification_queue.notifications + i;
            schedule_notification(n->appname, n->identifier, n->title, n->body, n->image_path, n->urgency, n->category_id, n->muted);
        }
    }
    while(notification_queue.count) {
        QueuedNotification *n = notification_queue.notifications + --notification_queue.count;
        if (!granted) do_notification_callback(n->identifier, "creation_failed", "");
        free(n->identifier); free(n->title); free(n->body); free(n->appname); free(n->image_path); free(n->category_id);
        memset(n, 0, sizeof(QueuedNotification));
    }
}

static PyObject*
cocoa_remove_delivered_notification(PyObject *self UNUSED, PyObject *x) {
    if (!PyUnicode_Check(x)) { PyErr_SetString(PyExc_TypeError, "identifier must be a string"); return NULL; }
    if (remove_delivered_notification(PyUnicode_AsUTF8(x))) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static PyObject*
cocoa_live_delivered_notifications(PyObject *self UNUSED, PyObject *x UNUSED) {
    if (live_delivered_notifications()) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static UNNotificationCategory*
category_from_python(PyObject *p) {
    RAII_PyObject(button_ids, PyObject_GetAttrString(p, "button_ids"));
    RAII_PyObject(buttons, PyObject_GetAttrString(p, "buttons"));
    RAII_PyObject(id, PyObject_GetAttrString(p, "id"));
    NSMutableArray<UNNotificationAction *> *actions = [NSMutableArray arrayWithCapacity:PyTuple_GET_SIZE(buttons)];
    for (int i = 0; i < PyTuple_GET_SIZE(buttons); i++) [actions addObject:
        [UNNotificationAction actionWithIdentifier:@(PyUnicode_AsUTF8(PyTuple_GET_ITEM(button_ids, i)))
            title:@(PyUnicode_AsUTF8(PyTuple_GET_ITEM(buttons, i))) options:UNNotificationActionOptionNone]];

    return [UNNotificationCategory categoryWithIdentifier:@(PyUnicode_AsUTF8(id))
        actions:actions intentIdentifiers:@[] options:0];
}

static bool
set_notification_categories(UNUserNotificationCenter *center, PyObject *categories) {
    NSMutableArray<UNNotificationCategory *> *ans = [NSMutableArray arrayWithCapacity:PyTuple_GET_SIZE(categories)];
    for (int i = 0; i < PyTuple_GET_SIZE(categories); i++) {
        UNNotificationCategory *c = category_from_python(PyTuple_GET_ITEM(categories, i));
        if (!c) return false;
        [ans addObject:c];
    }
    [center setNotificationCategories:[NSSet setWithArray:ans]];
    return true;
}

static PyObject*
cocoa_send_notification(PyObject *self UNUSED, PyObject *args, PyObject *kw) {
    const char *identifier = "", *title = "", *body = "", *appname = "", *image_path = ""; int urgency = 1;
    PyObject *category, *categories; int muted = 0;
    static const char* kwlist[] = {"appname", "identifier", "title", "body", "category", "categories", "image_path", "urgency", "muted", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "ssssOO!|sip", (char**)kwlist,
        &appname, &identifier, &title, &body, &category, &PyTuple_Type, &categories, &image_path, &urgency, &muted)) return NULL;

    UNUserNotificationCenter *center = get_notification_center_safely();
    if (!center) Py_RETURN_NONE;
    if (!center.delegate) center.delegate = [[NotificationDelegate alloc] init];
    if (PyObject_IsTrue(categories)) if (!set_notification_categories(center, categories)) return NULL;
    RAII_PyObject(category_id, PyObject_GetAttrString(category, "id"));
    queue_notification(appname, identifier, title, body, image_path, urgency, PyUnicode_AsUTF8(category_id), muted);

    // The badge permission needs to be requested as well, even though it is not used,
    // otherwise macOS refuses to show the preference checkbox for enable/disable notification sound.
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
        completionHandler:^(BOOL granted, NSError * _Nullable error) {
            if (!granted && error != nil) {
                log_error("Failed to request permission for showing notification: %s", [[error localizedDescription] UTF8String]);
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                drain_pending_notifications(granted);
            });
        }
    ];
    Py_RETURN_NONE;
}

@interface ServiceProvider : NSObject
@end

@implementation ServiceProvider

- (BOOL)openTab:(NSPasteboard*)pasteboard
        userData:(NSString *) UNUSED userData error:(NSError **) UNUSED error {
    return [self openDirsFromPasteboard:pasteboard type:NEW_TAB_WITH_WD];
}

- (BOOL)openOSWindow:(NSPasteboard*)pasteboard
        userData:(NSString *) UNUSED userData  error:(NSError **) UNUSED error {
    return [self openDirsFromPasteboard:pasteboard type:NEW_OS_WINDOW_WITH_WD];
}

- (BOOL)openDirsFromPasteboard:(NSPasteboard *)pasteboard type:(int)type {
    NSDictionary *options = @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES };
    NSArray *filePathArray = [pasteboard readObjectsForClasses:[NSArray arrayWithObject:[NSURL class]] options:options];
    NSMutableArray<NSString*> *dirPathArray = [NSMutableArray arrayWithCapacity:[filePathArray count]];
    for (NSURL *url in filePathArray) {
        NSString *path = [url path];
        BOOL isDirectory = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDirectory]) {
            if (!isDirectory) path = [path stringByDeletingLastPathComponent];
            if (![dirPathArray containsObject:path]) [dirPathArray addObject:path];
        }
    }
    if ([dirPathArray count] > 0) {
        // Colons are not valid in paths under macOS.
        set_cocoa_pending_action(type, [[dirPathArray componentsJoinedByString:@":"] UTF8String]);
    }
    return YES;
}

- (BOOL)openFileURLs:(NSPasteboard*)pasteboard
        userData:(NSString *) UNUSED userData  error:(NSError **) UNUSED error {
    NSDictionary *options = @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES };
    NSArray *urlArray = [pasteboard readObjectsForClasses:[NSArray arrayWithObject:[NSURL class]] options:options];
    for (NSURL *url in urlArray) {
        NSString *path = [url path];
        if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
            set_cocoa_pending_action(LAUNCH_URLS, [[[NSURL fileURLWithPath:path] absoluteString] UTF8String]);
        }
    }
    return YES;
}

- (void)quickAccessTerminal:(NSPasteboard *)pboard userData:(NSString *)userData error:(NSString **)error {
    // we ignore event during application launch as it will cause the window to be shown and hidden
    static bool is_first_event = true;
    if (!is_first_event || monotonic() >= s_double_to_monotonic_t(2.0)) { call_boss(quick_access_terminal_invoked, NULL); }
    is_first_event = false;
}
@end

// global menu {{{

static void
add_user_global_menu_entry(struct MenuItem *e, NSMenu *bar, size_t action_index) {
    NSMenu *parent = bar;
    UserMenuItem *final_item = nil;
    GlobalMenuTarget *global_menu_target = [GlobalMenuTarget shared_instance];
    for (size_t i = 0; i < e->location_count; i++) {
        NSMenuItem *item = [parent itemWithTitle:@(e->location[i])];
        if (!item) {
            final_item = [[UserMenuItem alloc] initWithTitle:@(e->location[i]) action:@selector(user_menu_action:) keyEquivalent:@""];
            final_item.target = global_menu_target;
            [parent addItem:final_item];
            item = final_item;
            [final_item release];
        }
        if (i + 1 < e->location_count) {
            if (![item hasSubmenu]) {
                NSMenu* sub_menu = [[NSMenu alloc] initWithTitle:item.title];
                [item setSubmenu:sub_menu];
                [sub_menu release];
            }
            parent = [item submenu];
            if (!parent) return;
        }
    }
    if (final_item != nil) {
        final_item.action_index = action_index;
    }
}

static void
cocoa_create_global_menu(void) {
    NSString* app_name = find_app_name();
    NSMenu* bar = [[NSMenu alloc] init];
    GlobalMenuTarget *global_menu_target = [GlobalMenuTarget shared_instance];
    [NSApp setMainMenu:bar];

#define MENU_ITEM(menu, title, name) { \
    NSMenuItem *__mi = [menu addItemWithTitle:title action:@selector(name:) keyEquivalent:@(global_shortcuts.name.key)]; \
    [__mi setKeyEquivalentModifierMask:global_shortcuts.name.mods]; \
    [__mi setTarget:global_menu_target]; \
}

    NSMenuItem* appMenuItem =
        [bar addItemWithTitle:@""
                       action:NULL
                keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenuItem setSubmenu:appMenu];

    [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %@", app_name]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(appMenu, @"Preferences…", edit_config_file);
    MENU_ITEM(appMenu, @"Reload Preferences", reload_config);
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu* servicesMenu = [[NSMenu alloc] init];
    [NSApp setServicesMenu:servicesMenu];
    [[appMenu addItemWithTitle:@"Services"
                        action:NULL
                 keyEquivalent:@""] setSubmenu:servicesMenu];
    [servicesMenu release];
    [appMenu addItem:[NSMenuItem separatorItem]];

    MENU_ITEM(appMenu, ([NSString stringWithFormat:@"Hide %@", app_name]), hide_macos_app);
    MENU_ITEM(appMenu, @"Hide Others", hide_macos_other_apps);
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    MENU_ITEM(appMenu, @"Secure Keyboard Entry", toggle_macos_secure_keyboard_entry);
    [appMenu addItem:[NSMenuItem separatorItem]];

    MENU_ITEM(appMenu, ([NSString stringWithFormat:@"Quit %@", app_name]), quit);
    [appMenu release];

    NSMenuItem* shellMenuItem = [bar addItemWithTitle:@"Shell" action:NULL keyEquivalent:@""];
    NSMenu* shellMenu = [[NSMenu alloc] initWithTitle:@"Shell"];
    [shellMenuItem setSubmenu:shellMenu];
    MENU_ITEM(shellMenu, @"New OS Window", new_os_window);
    MENU_ITEM(shellMenu, @"New Tab", new_tab);
    MENU_ITEM(shellMenu, @"New Window", new_window);
    [shellMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(shellMenu, @"Close OS Window", close_os_window);
    MENU_ITEM(shellMenu, @"Close Tab", close_tab);
    MENU_ITEM(shellMenu, @"Close Window", close_window);
    [shellMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(shellMenu, @"Reset", reset_terminal);
    [shellMenu release];
    NSMenuItem* editMenuItem = [bar addItemWithTitle:@"Edit" action:NULL keyEquivalent:@""];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenuItem setSubmenu:editMenu];
    MENU_ITEM(editMenu, @"Clear to Start", clear_terminal_and_scrollback);
    MENU_ITEM(editMenu, @"Clear Scrollback", clear_scrollback);
    MENU_ITEM(editMenu, @"Clear Screen", clear_screen);
    MENU_ITEM(editMenu, @"Clear Last Command", clear_last_command);
    MENU_ITEM(editMenu, @"Find", search_scrollback);
    [editMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(editMenu, @"Copy", copy_or_noop);
    MENU_ITEM(editMenu, @"Paste", paste_from_clipboard);
    [editMenu release];

    NSMenuItem* windowMenuItem =
        [bar addItemWithTitle:@"Window"
                       action:NULL
                keyEquivalent:@""];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenuItem setSubmenu:windowMenu];

    MENU_ITEM(windowMenu, @"Minimize", minimize_macos_window);
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(windowMenu, @"Cycle Through OS Windows", macos_cycle_through_os_windows);
    MENU_ITEM(windowMenu, @"Cycle Through OS Windows backwards", macos_cycle_through_os_windows_backwards);
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];

    [windowMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(windowMenu, @"Show Previous Tab", previous_tab);
    MENU_ITEM(windowMenu, @"Show Next Tab", next_tab);
    [[windowMenu addItemWithTitle:@"Move Tab to New Window"
                           action:@selector(detach_tab:)
                    keyEquivalent:@""] setTarget:global_menu_target];

    [windowMenu addItem:[NSMenuItem separatorItem]];
    MENU_ITEM(windowMenu, @"Enter Full Screen", toggle_fullscreen);
    [NSApp setWindowsMenu:windowMenu];
    [windowMenu release];

    NSMenuItem* helpMenuItem =
        [bar addItemWithTitle:@"Help"
                       action:NULL
                keyEquivalent:@""];
    NSMenu* helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    [helpMenuItem setSubmenu:helpMenu];

    MENU_ITEM(helpMenu, @"Visit kitty Website", open_kitty_website);
    [NSApp setHelpMenu:helpMenu];
    [helpMenu release];

    if (OPT(global_menu.entries)) {
        for (size_t i = 0; i < OPT(global_menu.count); i++) {
            struct MenuItem *e = OPT(global_menu.entries) + i;
            if (e->definition && e->location && e->location_count > 1) {
                add_user_global_menu_entry(e, bar, i);
            }
        }
    }
    [bar release];


    class_addMethod(
        object_getClass([NSApp delegate]),
        @selector(applicationDockMenu:),
        (IMP)get_dock_menu,
        "@@:@");


    [NSApp setServicesProvider:[[[ServiceProvider alloc] init] autorelease]];

#undef MENU_ITEM
}

void
cocoa_application_lifecycle_event(bool application_launch_finished) {
    if (application_launch_finished) {  // applicationDidFinishLaunching
        application_has_finished_launching = true;
    } else cocoa_create_global_menu();  // applicationWillFinishLaunching
}

void
cocoa_update_menu_bar_title(PyObject *pytitle) {
    if (!pytitle) return;
    NSString *title = nil;
    if (OPT(macos_menubar_title_max_length) > 0 && PyUnicode_GetLength(pytitle) > OPT(macos_menubar_title_max_length)) {
        static char fmt[64];
        snprintf(fmt, sizeof(fmt), "%%%ld.%ldU%%s", OPT(macos_menubar_title_max_length), OPT(macos_menubar_title_max_length));
        RAII_PyObject(st, PyUnicode_FromFormat(fmt, pytitle, "…"));
        if (st) title = @(PyUnicode_AsUTF8(st));
        else PyErr_Print();
    } else {
        title = @(PyUnicode_AsUTF8(pytitle));
    }
    if (!title) return;
    NSString *menuTitle = [NSString stringWithFormat:@" :: %@", title];
    if (title_menu != NULL) {
        [[title_menu submenu] setTitle:menuTitle];
    } else {
        NSMenu *bar = [NSApp mainMenu];
        title_menu = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
        NSMenu *m = [[NSMenu alloc] initWithTitle:menuTitle];
        [title_menu setSubmenu:m];
        [m release];
    }
}

void
cocoa_clear_global_shortcuts(void) {
    memset(&global_shortcuts, 0, sizeof(global_shortcuts));
}

void
cocoa_recreate_global_menu(void) {
    if (title_menu != NULL) {
        NSMenu *bar = [NSApp mainMenu];
        [bar removeItem:title_menu];
    }
    title_menu = NULL;
    if (secure_input_title_menu != NULL) {
        NSMenu *bar = [NSApp mainMenu];
        [bar removeItem:secure_input_title_menu];
    }
    secure_input_title_menu = NULL;
    cocoa_create_global_menu();
    SecureKeyboardEntryController *k = [SecureKeyboardEntryController sharedInstance];
    update_secure_input_menu_bar_indicator(k.isDesired);
}


// }}}

#define NSLeftAlternateKeyMask  (0x000020 | NSEventModifierFlagOption)
#define NSRightAlternateKeyMask (0x000040 | NSEventModifierFlagOption)

bool
cocoa_alt_option_key_pressed(NSUInteger flags) {
    NSUInteger q = (OPT(macos_option_as_alt) == 1) ? NSRightAlternateKeyMask : NSLeftAlternateKeyMask;
    return (q & flags) == q;
}

void
cocoa_toggle_secure_keyboard_entry(void) {
    SecureKeyboardEntryController *k = [SecureKeyboardEntryController sharedInstance];
    [k toggle];
    [[NSUserDefaults standardUserDefaults] setBool:k.isDesired forKey:@"SecureKeyboardEntry"];
    update_secure_input_menu_bar_indicator(k.isDesired);
}

void
cocoa_hide(void) {
    [[NSApplication sharedApplication] performSelectorOnMainThread:@selector(hide:) withObject:nil waitUntilDone:NO];
}

void
cocoa_hide_others(void) {
    [[NSApplication sharedApplication] performSelectorOnMainThread:@selector(hideOtherApplications:) withObject:nil waitUntilDone:NO];
}

void
cocoa_minimize(void *w) {
    NSWindow *window = (NSWindow*)w;
    if (window && !window.miniaturized) [window performSelectorOnMainThread:@selector(performMiniaturize:) withObject:nil waitUntilDone:NO];
}

void
cocoa_focus_window(void *w) {
    NSWindow *window = (NSWindow*)w;
    [window makeKeyWindow];
}

long
cocoa_window_number(void *w) {
    NSWindow *window = (NSWindow*)w;
    return [window windowNumber];
}

size_t
cocoa_get_workspace_ids(void *w, size_t *workspace_ids, size_t array_sz) {
    NSWindow *window = (NSWindow*)w;
    if (!window) return 0;
    NSArray *window_array = @[ @([window windowNumber]) ];
    CFArrayRef spaces = CGSCopySpacesForWindows(_CGSDefaultConnection(), kCGSSpaceAll, (__bridge CFArrayRef)window_array);
    CFIndex ans = CFArrayGetCount(spaces);
    if (ans > 0) {
        for (CFIndex i = 0; i < MIN(ans, (CFIndex)array_sz); i++) {
            NSNumber *s = (NSNumber*)CFArrayGetValueAtIndex(spaces, i);
            workspace_ids[i] = [s intValue];
        }
    } else ans = 0;
    CFRelease(spaces);
    return ans;
}

static PyObject*
cocoa_get_lang(PyObject UNUSED *self, PyObject *args UNUSED) {
    @autoreleasepool {
    NSString* lang_code = [[NSLocale currentLocale] languageCode];
    NSString* country_code = [[NSLocale currentLocale] objectForKey:NSLocaleCountryCode];
    NSString* identifier = [[NSLocale currentLocale] localeIdentifier];
    return Py_BuildValue("sss", lang_code ? [lang_code UTF8String]:"", country_code ? [country_code UTF8String] : "", identifier ? [identifier UTF8String]: "");
    } // autoreleasepool
}

monotonic_t
cocoa_cursor_blink_interval(void) {
    @autoreleasepool {

    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    double on_period_ms = [defaults doubleForKey:@"NSTextInsertionPointBlinkPeriodOn"];
    double off_period_ms = [defaults doubleForKey:@"NSTextInsertionPointBlinkPeriodOff"];
    double period_ms = [defaults doubleForKey:@"NSTextInsertionPointBlinkPeriod"];
    double max_value = 60 * 1000.0, ans = -1.0;
    if (on_period_ms != 0. || off_period_ms != 0.) {
        ans = on_period_ms + off_period_ms;
    } else if (period_ms != 0.) {
        ans = period_ms;
    }
    return ans > max_value ? 0ll : ms_double_to_monotonic_t(ans);

    } // autoreleasepool
}

void
cocoa_set_activation_policy(bool hide_from_tasks) {
    [NSApp setActivationPolicy:(hide_from_tasks ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular)];
}

static PyObject*
cocoa_set_url_handler(PyObject UNUSED *self, PyObject *args) {
    @autoreleasepool {

    const char *url_scheme = NULL, *bundle_id = NULL;
    if (!PyArg_ParseTuple(args, "s|z", &url_scheme, &bundle_id)) return NULL;
    if (!url_scheme || url_scheme[0] == '\0') {
        PyErr_SetString(PyExc_TypeError, "Empty url scheme");
        return NULL;
    }

    NSString *scheme = [NSString stringWithUTF8String:url_scheme];
    NSString *identifier = @"";
    if (!bundle_id) {
        identifier = [[NSBundle mainBundle] bundleIdentifier];
        if (!identifier || identifier.length == 0) identifier = @"net.kovidgoyal.kitty";
    } else if (bundle_id[0] != '\0') {
        identifier = [NSString stringWithUTF8String:bundle_id];
    }
    // This API has been marked as deprecated. It will need to be replaced when a new approach is available.
    OSStatus err = LSSetDefaultHandlerForURLScheme((CFStringRef)scheme, (CFStringRef)identifier);
    if (err == noErr) Py_RETURN_NONE;
    PyErr_Format(PyExc_OSError, "Failed to set default handler with error code: %d", err);
    return NULL;
    } // autoreleasepool
}

static PyObject*
cocoa_set_app_icon(PyObject UNUSED *self, PyObject *args) {
    @autoreleasepool {

    const char *icon_path = NULL, *app_path = NULL;
    if (!PyArg_ParseTuple(args, "s|z", &icon_path, &app_path)) return NULL;
    if (!icon_path || icon_path[0] == '\0') {
        PyErr_SetString(PyExc_TypeError, "Empty icon file path");
        return NULL;
    }
    NSString *custom_icon_path = [NSString stringWithUTF8String:icon_path];
    if (![[NSFileManager defaultManager] fileExistsAtPath:custom_icon_path]) {
        PyErr_Format(PyExc_FileNotFoundError, "Icon file not found: %s", [custom_icon_path UTF8String]);
        return NULL;
    }

    NSString *bundle_path = @"";
    if (!app_path) {
        bundle_path = [[NSBundle mainBundle] bundlePath];
        if (!bundle_path || bundle_path.length == 0) bundle_path = @"/Applications/kitty.app";
        // When compiled from source and run from the launcher folder the bundle path should be `kitty.app` in it
        if (![bundle_path hasSuffix:@".app"]) {
            NSString *launcher_app_path = [bundle_path stringByAppendingPathComponent:@"kitty.app"];
            bundle_path = @"";
            BOOL is_dir;
            if ([[NSFileManager defaultManager] fileExistsAtPath:launcher_app_path isDirectory:&is_dir] && is_dir && [[NSWorkspace sharedWorkspace] isFilePackageAtPath:launcher_app_path]) {
                bundle_path = launcher_app_path;
            }
        }
    } else if (app_path[0] != '\0') {
        bundle_path = [NSString stringWithUTF8String:app_path];
    }
    if (!bundle_path || bundle_path.length == 0 || ![[NSFileManager defaultManager] fileExistsAtPath:bundle_path]) {
        PyErr_Format(PyExc_FileNotFoundError, "Application bundle not found: %s", [bundle_path UTF8String]);
        return NULL;
    }

    NSImage *icon_image = [[NSImage alloc] initWithContentsOfFile:custom_icon_path];
    BOOL result = [[NSWorkspace sharedWorkspace] setIcon:icon_image forFile:bundle_path options:NSExcludeQuickDrawElementsIconCreationOption];
    [icon_image release];
    if (result) Py_RETURN_NONE;
    PyErr_Format(PyExc_OSError, "Failed to set custom icon %s for %s", [custom_icon_path UTF8String], [bundle_path UTF8String]);
    return NULL;

    } // autoreleasepool
}

static PyObject*
cocoa_set_dock_icon(PyObject UNUSED *self, PyObject *args) {
    @autoreleasepool {

    const char *icon_path = NULL;
    if (!PyArg_ParseTuple(args, "s", &icon_path)) return NULL;
    if (!icon_path || icon_path[0] == '\0') {
        PyErr_SetString(PyExc_TypeError, "Empty icon file path");
        return NULL;
    }
    NSString *custom_icon_path = [NSString stringWithUTF8String:icon_path];
    if ([[NSFileManager defaultManager] fileExistsAtPath:custom_icon_path]) {
        NSImage *icon_image = [[[NSImage alloc] initWithContentsOfFile:custom_icon_path] autorelease];
        [NSApplication sharedApplication].applicationIconImage = icon_image;
        Py_RETURN_NONE;
    }
    return NULL;

    } // autoreleasepool
}

static NSSound *beep_sound = nil;

static void
cleanup(void) {
    @autoreleasepool {

    if (dockMenu) [dockMenu release];
    dockMenu = nil;
    if (beep_sound) [beep_sound release];
    beep_sound = nil;

    drain_pending_notifications(NO);
    free(notification_queue.notifications);
    notification_queue.notifications = NULL;
    notification_queue.capacity = 0;

    } // autoreleasepool
}

void
cocoa_system_beep(const char *path) {
    if (!path) { NSBeep(); return; }
    static const char *beep_path = NULL;
    if (beep_path != path) {
        if (beep_sound) [beep_sound release];
        beep_sound = [[NSSound alloc] initWithContentsOfFile:@(path) byReference:YES];
    }
    if (beep_sound) [beep_sound play];
    else NSBeep();
}

static void
uncaughtExceptionHandler(NSException *exception) {
    log_error("Unhandled exception in Cocoa: %s", [[exception description] UTF8String]);
    log_error("Stack trace:\n%s", [[exception.callStackSymbols description] UTF8String]);
}

void
cocoa_set_uncaught_exception_handler(void) {
    NSSetUncaughtExceptionHandler(&uncaughtExceptionHandler);
}

static PyObject*
convert_imagerep_to_png(NSBitmapImageRep *rep, const char *output_path) {
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{NSImageCompressionFactor: @1.0}]; // autoreleased

    if (output_path) {
        if (![png writeToFile:@(output_path) atomically:YES]) {
            PyErr_Format(PyExc_OSError, "Failed to write PNG data to %s", output_path);
            return NULL;
        }
        return PyBytes_FromStringAndSize(NULL, 0);
    }
    return PyBytes_FromStringAndSize(png.bytes, png.length);
}

static PyObject*
convert_image_to_png(NSImage *icon, unsigned image_size, const char *output_path) {
    NSRect r = NSMakeRect(0, 0, image_size, image_size);
    RAII_CoreFoundation(CGColorSpaceRef, colorSpace, CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB));
    RAII_CoreFoundation(CGContextRef, cgContext, CGBitmapContextCreate(NULL, image_size, image_size, 8, 4*image_size, colorSpace, kCGBitmapByteOrderDefault|kCGImageAlphaPremultipliedLast));
    NSGraphicsContext *context = [NSGraphicsContext graphicsContextWithCGContext:cgContext flipped:NO];  // autoreleased
    CGImageRef cg = [icon CGImageForProposedRect:&r context:context hints:nil];
    NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc] initWithCGImage:cg] autorelease];
    return convert_imagerep_to_png(rep, output_path);
}

static PyObject*
render_emoji(NSString *text, unsigned image_size, const char *output_path) {
    NSFont *font = [NSFont fontWithName:@"AppleColorEmoji" size:12];
    CTFontRef ctfont = (__bridge CTFontRef)(font);
    CGFloat line_height = MAX(1, floor(CTFontGetAscent(ctfont) + CTFontGetDescent(ctfont) + MAX(0, CTFontGetLeading(ctfont)) + 0.5));
    CGFloat pts_per_px = CTFontGetSize(ctfont) / line_height;
    CGFloat desired_size = image_size * pts_per_px;
    NSFont *final_font = [NSFont fontWithName:@"AppleColorEmoji" size:desired_size];
    NSAttributedString *attr_string = [[[NSAttributedString alloc] initWithString:text attributes:@{NSFontAttributeName: final_font}] autorelease];
    NSBitmapImageRep *bmp = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:image_size pixelsHigh:image_size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0] autorelease];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext *context = [NSGraphicsContext graphicsContextWithBitmapImageRep:bmp];
    [NSGraphicsContext setCurrentContext:context];
    [attr_string drawInRect:NSMakeRect(0, 0, image_size, image_size)];
    [NSGraphicsContext restoreGraphicsState];
    return convert_imagerep_to_png(bmp, output_path);
}


static PyObject*
bundle_image_as_png(PyObject *self UNUSED, PyObject *args, PyObject *kw) {@autoreleasepool {
    const char *b, *output_path = NULL; int image_type = 1; unsigned image_size = 256;
    static const char* kwlist[] = {"path_or_identifier", "output_path", "image_size", "image_type", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|sIi", (char**)kwlist, &b, &output_path, &image_size, &image_type)) return NULL;
    NSImage *icon = nil;
    switch (image_type) {
        case 0: case 1: {
            NSWorkspace *workspace = [NSWorkspace sharedWorkspace]; // autoreleased
            if (image_type == 1) {
                NSURL *url = [workspace URLForApplicationWithBundleIdentifier:@(b)]; // autoreleased
                if (!url) {
                    PyErr_Format(PyExc_KeyError, "Failed to find bundle path for identifier: %s", b); return NULL;
                }
                icon = [workspace iconForFile:@(url.fileSystemRepresentation)];
            } else icon = [workspace iconForFile:@(b)];
        } break;
        case 2:
            return render_emoji(@(b), image_size, output_path);
        default:
            if (@available(macOS 11.0, *)) {
                icon = [NSImage imageWithSystemSymbolName:@(b) accessibilityDescription:@""];  // autoreleased
            } else {
                PyErr_SetString(PyExc_ValueError, "Your version of macOS is too old to use symbol images, need >= 11.0"); return NULL;
            }
            break;
    }
    if (!icon) {
        PyErr_Format(PyExc_ValueError, "Failed to load icon for bundle: %s", b); return NULL;
    }
    return convert_image_to_png(icon, image_size, output_path);
}}

static PyObject*
play_system_sound_by_id_async(PyObject *self UNUSED, PyObject *which) {
    if (!PyLong_Check(which)) { PyErr_SetString(PyExc_TypeError, "system sound id must be an integer"); return NULL; }
    AudioServicesPlaySystemSound(PyLong_AsUnsignedLong(which));
    Py_RETURN_NONE;
}

// Dock Progress bar {{{
@interface RoundedRectangleView : NSView {
    unsigned intermediate_step;
    CGFloat fill_fraction;
    BOOL is_indeterminate;
}
- (void) animate;
- (BOOL) isIndeterminate;
- (void) setIndeterminate:(BOOL)val;
- (void) setFraction:(CGFloat) fraction;
@end

@implementation RoundedRectangleView

- (void) animate { intermediate_step++; }
- (BOOL) isIndeterminate { return is_indeterminate; }
- (void) setIndeterminate:(BOOL)val {
    if (val != is_indeterminate) {
        is_indeterminate = val;
        intermediate_step = 0;
        }
    }
- (void) setFraction:(CGFloat)fraction { fill_fraction = fraction; }


- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];

    NSRect bar = NSInsetRect(self.bounds, 4, 4);
    CGFloat cornerRadius = self.bounds.size.height / 4.0;

#define fill(bar) [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:cornerRadius yRadius:cornerRadius] fill]
    // Create the border
    [[[NSColor whiteColor] colorWithAlphaComponent:0.8] setFill];
    fill(bar);
    // Create the background
    [[[NSColor blackColor] colorWithAlphaComponent:0.8] setFill];
    fill(NSInsetRect(bar, 0.5, 0.5));
    // Create the progress
    NSRect bar_progress = NSInsetRect(bar, 1, 1);
    if (intermediate_step) {
        unsigned num_of_steps = 80;
        intermediate_step = intermediate_step % num_of_steps;
        bar_progress.size.width = self.bounds.size.width / 8;
        float frac = intermediate_step / (float)num_of_steps;
        bar_progress.origin.x += (self.bounds.size.width - bar_progress.size.width) * frac;
    } else bar_progress.size.width *= fill_fraction;
    [[NSColor whiteColor] setFill];
    fill(bar_progress);
#undef fill
}

@end
static NSView *dock_content_view = nil;
static NSImageView *dock_image_view = nil;
static RoundedRectangleView *dock_pbar = nil;

static void
animate_dock_progress_bar(id_type timer_id UNUSED, void *data UNUSED);

static void
tick_dock_pbar(void) {
    add_main_loop_timer(ms_to_monotonic_t(20), false, animate_dock_progress_bar, NULL, NULL);
}

static void
animate_dock_progress_bar(id_type timer_id UNUSED, void *data UNUSED) {
    if (dock_pbar != nil && [dock_pbar isIndeterminate]) {
        [dock_pbar animate];
        NSDockTile *dockTile = [NSApp dockTile];
        [dockTile display];
        tick_dock_pbar();
    }
}

static PyObject*
cocoa_show_progress_bar_on_dock_icon(PyObject *self UNUSED, PyObject *args) {
    float percent = -100;
    if (!PyArg_ParseTuple(args, "|f", &percent)) return NULL;
    NSDockTile *dockTile = [NSApp dockTile];
    if (!dock_content_view) {
        dock_content_view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, dockTile.size.width, dockTile.size.height)];
        dock_image_view = [NSImageView.alloc initWithFrame:dock_content_view.frame];
        dock_image_view.imageScaling = NSImageScaleProportionallyDown;
        dock_image_view.image = NSApp.applicationIconImage;
        [dock_content_view addSubview:dock_image_view];
        dock_pbar = [[RoundedRectangleView alloc] initWithFrame:NSMakeRect(0, 0, dockTile.size.width, dockTile.size.height / 4)];
        [dock_content_view addSubview:dock_pbar];
    }
    [dock_content_view setFrameSize:dockTile.size];
    [dock_image_view setFrameSize:dockTile.size];
    if (percent >= 0 && percent <= 100) {
        [dock_pbar setFraction:percent/100.];
        [dock_pbar setIndeterminate:NO];
    } else if (percent > 100) {
        if (![dock_pbar isIndeterminate]) {
            [dock_pbar setIndeterminate:YES];
            tick_dock_pbar();
        }
    }
    [dock_pbar setFrameSize:NSMakeSize(dockTile.size.width - 20, 20)];
    [dock_pbar setFrameOrigin:NSMakePoint(10, -2)];
    [dockTile setContentView:percent < 0 ? nil : dock_content_view];
    [dockTile display];
    Py_RETURN_NONE;
}
// }}}

// Titlebar tab bar {{{

#define TITLEBAR_TABS_IDENTIFIER @"kitty-titlebar-tabs"
static const CGFloat kTabMaxWidth = 200.0, kTabMinWidth = 60.0, kTabHeight = 24.0, kTabSpacing = 4.0, kTabCornerRadius = 6.0;
static const CGFloat kTabBarLeftMargin = 84.0;  // clear the traffic light buttons
static const CGFloat kNewTabButtonWidth = 28.0;
static const NSTimeInterval kTabAnimationDuration = 0.18;
static const CGFloat kTabDetachMargin = 40.0;  // tear-off requires the drop point to be this far from the tab bar

static NSColor*
titlebar_tab_color_from_rgb(unsigned int c) {
    return [NSColor colorWithSRGBRed:((c >> 16) & 0xff) / 255.0 green:((c >> 8) & 0xff) / 255.0 blue:(c & 0xff) / 255.0 alpha:1.0];
}

static NSString *const KittyTitlebarTabPasteboardType = @"net.kovidgoyal.kitty.titlebar-tab";

@class KittyTitlebarTabView;
@class KittyTitlebarNewTabButton;

@interface KittyTitlebarTabBarView : NSView
@property (nonatomic) unsigned long long os_window_id;
@property (nonatomic, retain) KittyTitlebarNewTabButton *plusButton;
@property (nonatomic, assign) KittyTitlebarTabView *dragged_tab;
@property (nonatomic) NSUInteger drag_index;
@property (nonatomic) BOOL drop_was_internal;
@property (nonatomic) CGFloat last_drag_x;
@property (nonatomic) CGFloat last_tab_width;
- (void)layoutTabsAnimated:(BOOL)animated initialLayoutForNewTabs:(NSArray<KittyTitlebarTabView*>*)new_tabs draggedTab:(KittyTitlebarTabView*)dragged_tab dragIndex:(NSUInteger)drag_index;
- (void)positionDraggedTabAtPoint:(NSPoint)p;
@end

@interface KittyTitlebarTabView : NSView <NSDraggingSource> {
    NSTrackingArea *tracking_area;
    NSPoint mouse_down_location;
    BOOL drag_in_progress;
}
@property (nonatomic) unsigned long long tab_id;
@property (nonatomic) unsigned long long os_window_id;
@property (nonatomic) BOOL is_active;
@property (nonatomic) BOOL needs_attention;
@property (nonatomic) BOOL hovered;
@property (nonatomic) BOOL close_hovered;
@property (nonatomic) BOOL marked_for_removal;
@property (nonatomic) unsigned int fg_rgb;
@property (nonatomic) unsigned int bg_rgb;
@property (nonatomic, retain) NSTextField *titleField;
- (CGFloat)dragGrabOffsetX;
@end

@implementation KittyTitlebarTabView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = kTabCornerRadius;
        self.layer.masksToBounds = YES;
        NSTextField *tf = [[NSTextField alloc] initWithFrame:NSZeroRect];
        tf.editable = NO; tf.selectable = NO; tf.bordered = NO; tf.bezeled = NO;
        tf.drawsBackground = NO;
        tf.font = [NSFont systemFontOfSize:12];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.alignment = NSTextAlignmentCenter;
        self.titleField = tf;
        [self addSubview:tf];
        [tf release];
    }
    return self;
}

- (void)dealloc {
    if (tracking_area) { [self removeTrackingArea:tracking_area]; [tracking_area release]; tracking_area = nil; }
    self.titleField = nil;
    [super dealloc];
}

- (NSRect)closeButtonRect {
    return NSMakeRect(self.bounds.size.width - 20, (self.bounds.size.height - 14) / 2, 14, 14);
}

- (void)updateTrackingAreas {
    if (tracking_area) { [self removeTrackingArea:tracking_area]; [tracking_area release]; }
    tracking_area = [[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways
        owner:self userInfo:nil];
    [self addTrackingArea:tracking_area];
    [super updateTrackingAreas];
}

- (void)applyColorsAnimated:(BOOL)animated {
    NSColor *bg = titlebar_tab_color_from_rgb(self.bg_rgb);
    NSColor *fg = titlebar_tab_color_from_rgb(self.fg_rgb);
    if (self.hovered && !self.is_active) {
        CGFloat r = 0, g = 0, b = 0, a = 0;
        [bg getRed:&r green:&g blue:&b alpha:&a];
        const CGFloat luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        NSColor *toward = luminance < 0.5 ? [NSColor whiteColor] : [NSColor blackColor];
        bg = [bg blendedColorWithFraction:0.08 ofColor:toward];
    }
    if (self.needs_attention && !self.is_active) fg = [NSColor systemOrangeColor];
    if (animated) {
        CABasicAnimation *anim = [CABasicAnimation animationWithKeyPath:@"backgroundColor"];
        anim.duration = kTabAnimationDuration;
        anim.fromValue = (id)(self.layer.backgroundColor ? self.layer.backgroundColor : [NSColor clearColor].CGColor);
        anim.toValue = (id)bg.CGColor;
        [self.layer addAnimation:anim forKey:@"bgcolor"];
    }
    self.layer.backgroundColor = bg.CGColor;
    self.titleField.textColor = fg;
    self.titleField.font = [NSFont systemFontOfSize:12];
    [self setNeedsDisplay:YES];
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyColorsAnimated:NO];
}

- (void)layout {
    [super layout];
    self.titleField.frame = NSMakeRect(8, (self.bounds.size.height - 16) / 2 - 1, MAX(0, self.bounds.size.width - 8 - 22), 16);
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    NSRect cr = [self closeButtonRect];
    NSColor *fg = titlebar_tab_color_from_rgb(self.fg_rgb);
    if (self.close_hovered) {
        [[fg colorWithAlphaComponent:0.25] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:cr] fill];
    }
    NSColor *xcolor = self.close_hovered ? fg : [fg colorWithAlphaComponent:0.6];
    [xcolor setStroke];
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 1.2;
    const CGFloat inset = 4.25;
    [p moveToPoint:NSMakePoint(NSMinX(cr) + inset, NSMinY(cr) + inset)];
    [p lineToPoint:NSMakePoint(NSMaxX(cr) - inset, NSMaxY(cr) - inset)];
    [p moveToPoint:NSMakePoint(NSMaxX(cr) - inset, NSMinY(cr) + inset)];
    [p lineToPoint:NSMakePoint(NSMinX(cr) + inset, NSMaxY(cr) - inset)];
    [p stroke];
}

- (void)mouseEntered:(NSEvent *)event {
    (void)event;
    self.hovered = YES;
    [self applyColorsAnimated:YES];
    self.needsLayout = YES;
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    self.hovered = NO; self.close_hovered = NO;
    [self applyColorsAnimated:YES];
    self.needsLayout = YES;
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint pos = [self convertPoint:event.locationInWindow fromView:nil];
    BOOL over_close = NSPointInRect(pos, NSInsetRect([self closeButtonRect], -2, -2));
    if (over_close != self.close_hovered) {
        self.close_hovered = over_close;
        [self setNeedsDisplay:YES];
    }
}

- (void)sendAction:(CocoaPendingAction)action {
    char payload[128];
    snprintf(payload, sizeof(payload), "%llu %llu", self.os_window_id, self.tab_id);
    set_cocoa_pending_action(action, payload);
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)mouseDown:(NSEvent *)event {
    mouse_down_location = [self convertPoint:event.locationInWindow fromView:nil];
    drag_in_progress = NO;
}

- (CGFloat)dragGrabOffsetX {
    return mouse_down_location.x;
}

- (void)mouseDragged:(NSEvent *)event {
    if (drag_in_progress || self.marked_for_removal) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (fabs(p.x - mouse_down_location.x) < 4 && fabs(p.y - mouse_down_location.y) < 4) return;
    NSPasteboardItem *pb = [[NSPasteboardItem alloc] init];
    [pb setString:[NSString stringWithFormat:@"%llu %llu", self.os_window_id, self.tab_id] forType:KittyTitlebarTabPasteboardType];
    NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:pb];
    // a fully transparent image of the tab's size keeps the system from
    // auto-capturing the view (which would follow the cursor as a
    // semi-transparent tab); the tab view itself is the visible ghost,
    // lifted above its siblings so its Y stays locked to the tab bar
    NSImage *empty = [[[NSImage alloc] initWithSize:self.bounds.size] autorelease];
    [empty lockFocus];
    [[NSColor clearColor] setFill];
    NSRectFill(NSMakeRect(0, 0, self.bounds.size.width, self.bounds.size.height));
    [empty unlockFocus];
    [item setDraggingFrame:self.bounds contents:empty];
    NSDraggingSession *session = [self beginDraggingSessionWithItems:@[item] event:event source:self];
    session.animatesToStartingPositionsOnCancelOrFail = NO;
    [item release];
    [pb release];
    drag_in_progress = YES;
    [self.superview addSubview:self];  // lift the ghost above its siblings
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session; (void)context;
    return NSDragOperationMove;
}

- (void)draggingSession:(NSDraggingSession *)session movedToPoint:(NSPoint)screenPoint {
    (void)session; (void)screenPoint;
    // called for every mouse move during the drag, even outside the tab
    // bar; the ghost follows the cursor once torn off and snaps back to
    // the bar as soon as the cursor returns within the tear-off margin
    KittyTitlebarTabBarView *bar = (KittyTitlebarTabBarView*)self.superview;
    if (![bar isKindOfClass:[KittyTitlebarTabBarView class]] || bar.dragged_tab != self) return;
    NSPoint p = [bar convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
    [bar positionDraggedTabAtPoint:p];
}

- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)screenPoint operation:(NSDragOperation)operation {
    (void)session;
    drag_in_progress = NO;
    KittyTitlebarTabBarView *bar = (KittyTitlebarTabBarView*)self.superview;
    if ([bar isKindOfClass:[KittyTitlebarTabBarView class]]) {
        NSUInteger last_index = bar.drag_index;
        BOOL dropped_here = bar.drop_was_internal;
        bar.dragged_tab = nil;
        bar.drag_index = NSNotFound;
        bar.drop_was_internal = NO;
        if (operation == NSDragOperationNone && !self.marked_for_removal) {
            // like Chrome, only tear the tab off when the drop point is
            // well outside the tab bar; releasing just below it commits
            // the reorder at the last position instead. The drop point is
            // the current mouse location in the window, measured in the
            // bar's own coordinates, which avoids coordinate ambiguity.
            NSPoint p = [bar convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
            BOOL tear_off = p.y < -kTabDetachMargin || p.y > bar.bounds.size.height + kTabDetachMargin;
            if (tear_off) {
                char payload[160];
                snprintf(payload, sizeof(payload), "%llu %llu %d %d", self.os_window_id, self.tab_id, (int)screenPoint.x, (int)screenPoint.y);
                set_cocoa_pending_action(TITLEBAR_TAB_DETACH, payload);
            } else if (last_index != NSNotFound) {
                // land the ghost into the gap, then commit the reorder
                [bar layoutTabsAnimated:YES initialLayoutForNewTabs:nil draggedTab:self dragIndex:last_index];
                char payload[224];
                snprintf(payload, sizeof(payload), "%llu %llu %llu %u", self.os_window_id, self.tab_id, self.os_window_id, (unsigned)last_index);
                set_cocoa_pending_action(TITLEBAR_TAB_DROP, payload);
            }
        } else if (operation == NSDragOperationMove && dropped_here && !self.marked_for_removal) {
            // dropped back into this window's tab bar: land into the gap
            [bar layoutTabsAnimated:YES initialLayoutForNewTabs:nil draggedTab:self dragIndex:last_index];
        }
    }
}

- (void)mouseUp:(NSEvent *)event {
    if (self.marked_for_removal || drag_in_progress) return;
    NSPoint pos = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(pos, self.bounds)) return;
    if (NSPointInRect(pos, NSInsetRect([self closeButtonRect], -2, -2))) {
        [self sendAction:TITLEBAR_TAB_CLOSE];
    } else {
        [self sendAction:TITLEBAR_TAB_ACTIVATE];
    }
}

- (void)otherMouseUp:(NSEvent *)event {
    if (event.buttonNumber == 2) [self sendAction:TITLEBAR_TAB_CLOSE];
}

@end

@interface KittyTitlebarNewTabButton : NSView {
    NSTrackingArea *tracking_area;
}
@property (nonatomic) unsigned long long os_window_id;
@property (nonatomic) BOOL hovered;
@end

@implementation KittyTitlebarNewTabButton

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = kTabCornerRadius;
    }
    return self;
}

- (void)dealloc {
    if (tracking_area) { [self removeTrackingArea:tracking_area]; [tracking_area release]; tracking_area = nil; }
    [super dealloc];
}

- (void)updateTrackingAreas {
    if (tracking_area) { [self removeTrackingArea:tracking_area]; [tracking_area release]; }
    tracking_area = [[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways owner:self userInfo:nil];
    [self addTrackingArea:tracking_area];
    [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    BOOL dark = NO;
    NSAppearanceName name = [self.effectiveAppearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    dark = [name isEqualToString:NSAppearanceNameDarkAqua];
    if (self.hovered) {
        NSColor *bg = dark ? [NSColor colorWithWhite:1.0 alpha:0.12] : [NSColor colorWithWhite:0.0 alpha:0.08];
        [bg setFill];
        [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:kTabCornerRadius yRadius:kTabCornerRadius] fill];
    }
    [[NSColor secondaryLabelColor] setStroke];
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 1.2;
    NSPoint c = NSMakePoint(NSMidX(self.bounds), NSMidY(self.bounds));
    const CGFloat arm = 5.0;
    [p moveToPoint:NSMakePoint(c.x - arm, c.y)];
    [p lineToPoint:NSMakePoint(c.x + arm, c.y)];
    [p moveToPoint:NSMakePoint(c.x, c.y - arm)];
    [p lineToPoint:NSMakePoint(c.x, c.y + arm)];
    [p stroke];
}

- (void)mouseEntered:(NSEvent *)event { (void)event; self.hovered = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent *)event { (void)event; self.hovered = NO; [self setNeedsDisplay:YES]; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (void)mouseDown:(NSEvent *)event { (void)event; }

- (void)mouseUp:(NSEvent *)event {
    NSPoint pos = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(pos, self.bounds)) return;
    char payload[64];
    snprintf(payload, sizeof(payload), "%llu 0", self.os_window_id);
    set_cocoa_pending_action(TITLEBAR_TAB_NEW, payload);
}

@end

@implementation KittyTitlebarTabBarView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.identifier = TITLEBAR_TABS_IDENTIFIER;
        self.drag_index = NSNotFound;
        KittyTitlebarNewTabButton *b = [[KittyTitlebarNewTabButton alloc] initWithFrame:NSZeroRect];
        self.plusButton = b;
        [self addSubview:b];
        [b release];
        [self registerForDraggedTypes:@[KittyTitlebarTabPasteboardType]];
    }
    return self;
}

- (void)dealloc {
    self.plusButton = nil;
    [super dealloc];
}

// empty areas of the bar still drag the window, while remaining a valid
// drop target for tab drags
- (BOOL)mouseDownCanMoveWindow { return YES; }

- (NSUInteger)dropIndexForPoint:(NSPoint)p {
    NSUInteger idx = 0;
    KittyTitlebarTabView *dragged = self.dragged_tab;
    if (dragged) {
        // like Chrome, swap once the ghost covers more than half of the
        // neighboring tab. The ghost follows the cursor from the point
        // where the tab was grabbed, so use its real edges, not the
        // cursor position. The dragged tab itself is excluded, otherwise
        // the cursor crossing its own midpoint would flip the index back
        // and forth.
        const BOOL moving_right = p.x >= self.last_drag_x;
        const CGFloat ghost_left = p.x - [dragged dragGrabOffsetX];
        const CGFloat ghost_right = ghost_left + self.last_tab_width;
        for (KittyTitlebarTabView *tab in [self tabViews]) {
            if (tab == dragged) continue;
            if (moving_right ? ghost_right > NSMidX(tab.frame) : NSMidX(tab.frame) < ghost_left) idx++;
        }
    } else {
        for (KittyTitlebarTabView *tab in [self tabViews]) {
            if (p.x > NSMidX(tab.frame)) idx++;
        }
    }
    return idx;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSString *payload = [sender.draggingPasteboard stringForType:KittyTitlebarTabPasteboardType];
    if (!payload) return NSDragOperationNone;
    self.drop_was_internal = NO;
    self.dragged_tab = nil;
    self.drag_index = NSNotFound;
    self.last_drag_x = [self convertPoint:sender.draggingLocation fromView:nil].x;
    NSArray<NSString*> *parts = [payload componentsSeparatedByString:@" "];
    if (parts.count > 1) {
        unsigned long long tid = [parts[1] longLongValue];
        NSUInteger i = 0;
        for (KittyTitlebarTabView *tab in [self tabViews]) {
            if (tab.tab_id == tid) { self.dragged_tab = tab; self.drag_index = i; break; }
            i++;
        }
    }
    return NSDragOperationMove;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    NSString *payload = [sender.draggingPasteboard stringForType:KittyTitlebarTabPasteboardType];
    if (!payload) return NSDragOperationNone;
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];
    if (self.dragged_tab) {
        NSUInteger idx = [self dropIndexForPoint:p];
        if (idx != self.drag_index) {
            self.drag_index = idx;
            [self layoutTabsAnimated:YES initialLayoutForNewTabs:nil draggedTab:self.dragged_tab dragIndex:idx];
        }
    }
    self.last_drag_x = p.x;
    return NSDragOperationMove;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    // the reflowed layout is kept so that releasing just outside the bar
    // commits the reorder, and tearing off lets Python close the gap
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSString *payload = [sender.draggingPasteboard stringForType:KittyTitlebarTabPasteboardType];
    if (!payload) return NO;
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];
    unsigned int idx = (unsigned int)[self dropIndexForPoint:p];
    NSArray<NSString*> *parts = [payload componentsSeparatedByString:@" "];
    self.drop_was_internal = NO;
    if (parts.count > 1) {
        unsigned long long tid = [parts[1] longLongValue];
        for (KittyTitlebarTabView *tab in [self tabViews]) {
            if (tab.tab_id == tid) { self.drop_was_internal = YES; break; }
        }
    }
    // keep dragged_tab/drag_index until the source's draggingSession:ended
    // reads them to land the ghost into the gap
    char buf[224];
    snprintf(buf, sizeof(buf), "%s %llu %u", payload.UTF8String, self.os_window_id, idx);
    set_cocoa_pending_action(TITLEBAR_TAB_DROP, buf);
    return YES;
}

- (NSArray<KittyTitlebarTabView *> *)tabViews {
    NSMutableArray<KittyTitlebarTabView *> *ans = [NSMutableArray array];
    for (NSView *v in self.subviews) {
        if ([v isKindOfClass:[KittyTitlebarTabView class]] && !((KittyTitlebarTabView*)v).marked_for_removal) [ans addObject:(KittyTitlebarTabView*)v];
    }
    return ans;
}

- (CGFloat)titlebarCenterY {
    CGFloat center_y = NSMidY(self.bounds);
    NSWindow *window = self.window;
    if (window) {
        const CGFloat titlebar_bottom_win = NSMaxY(window.contentLayoutRect);
        const CGFloat titlebar_top_win = window.frame.size.height;
        if (titlebar_top_win > titlebar_bottom_win) {
            NSPoint p = [self convertPoint:NSMakePoint(0, (titlebar_bottom_win + titlebar_top_win) / 2.0) fromView:nil];
            center_y = p.y;
        }
    }
    return center_y;
}

// the dragged tab acts as the ghost: it follows the cursor horizontally but
// stays locked to the tab bar's Y, like Chrome. Once the cursor leaves the
// bar beyond the tear-off margin it follows the cursor vertically too,
// driven by draggingSession:movedToPoint: on the source view.
- (void)positionDraggedTabAtPoint:(NSPoint)p {
    KittyTitlebarTabView *tab = self.dragged_tab;
    if (!tab) return;
    if (self.subviews.lastObject != tab) [self addSubview:tab];  // keep the ghost on top
    CGFloat ty = [self titlebarCenterY] - kTabHeight / 2.0;
    if (p.y < -kTabDetachMargin || p.y > self.bounds.size.height + kTabDetachMargin) {
        ty = p.y - kTabHeight / 2.0;
    }
    [tab setFrame:NSMakeRect(p.x - [tab dragGrabOffsetX], ty, self.last_tab_width, kTabHeight)];
}

- (void)layoutTabsAnimated:(BOOL)animated initialLayoutForNewTabs:(NSArray<KittyTitlebarTabView*>*)new_tabs {
    [self layoutTabsAnimated:animated initialLayoutForNewTabs:new_tabs draggedTab:self.dragged_tab dragIndex:self.drag_index];
}

- (void)layoutTabsAnimated:(BOOL)animated initialLayoutForNewTabs:(NSArray<KittyTitlebarTabView*>*)new_tabs draggedTab:(KittyTitlebarTabView*)dragged_tab dragIndex:(NSUInteger)drag_index {
    NSMutableArray<KittyTitlebarTabView *> *tabs = [NSMutableArray arrayWithArray:[self tabViews]];
    if (dragged_tab && drag_index != NSNotFound && [tabs containsObject:dragged_tab]) {
        [tabs removeObject:dragged_tab];
        if (drag_index > tabs.count) drag_index = tabs.count;
        [tabs insertObject:dragged_tab atIndex:drag_index];
    }
    // while a drag is active the dragged tab is a floating ghost positioned
    // by positionDraggedTabAtPoint, so the layout leaves its slot empty
    const BOOL ghost_mode = (dragged_tab && self.dragged_tab == dragged_tab);
    const CGFloat available = self.bounds.size.width - kNewTabButtonWidth - kTabSpacing;
    const NSUInteger n = tabs.count;
    CGFloat tab_width = kTabMaxWidth;
    if (n > 0) tab_width = MIN(kTabMaxWidth, MAX(kTabMinWidth, (available - kTabSpacing * (n - 1)) / n));
    self.last_tab_width = tab_width;
    // center the tabs vertically in the visible titlebar area, which spans
    // from the top of the content area to the top of the window
    const CGFloat y = [self titlebarCenterY] - kTabHeight / 2.0;
    CGFloat x = 0;
    void (^apply)(void) = ^{
        CGFloat cx = x;
        for (KittyTitlebarTabView *tab in tabs) {
            if (ghost_mode && tab == dragged_tab) { cx += tab_width + kTabSpacing; continue; }
            NSRect frame = NSMakeRect(cx, y, tab_width, kTabHeight);
            if (animated && ![new_tabs containsObject:tab]) {
                [[tab animator] setFrame:frame];
            } else {
                tab.frame = frame;
                if ([new_tabs containsObject:tab] && animated) {
                    tab.alphaValue = 0.0;
                    [[tab animator] setAlphaValue:1.0];
                }
            }
            tab.needsLayout = YES;
            cx += tab_width + kTabSpacing;
        }
        NSRect btn_frame = NSMakeRect(cx, y, kNewTabButtonWidth, kTabHeight);
        if (animated) [[self.plusButton animator] setFrame:btn_frame];
        else self.plusButton.frame = btn_frame;
    };
    if (animated) {
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = kTabAnimationDuration;
            ctx.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
            apply();
        }];
    } else apply();
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
    [super resizeSubviewsWithOldSize:oldSize];
    [self layoutTabsAnimated:NO initialLayoutForNewTabs:nil];
}

- (void)updateWithTabs:(const TitlebarTabInfo *)infos count:(size_t)count {
    self.plusButton.os_window_id = self.os_window_id;
    NSMutableDictionary<NSNumber*, KittyTitlebarTabView*> *existing = [NSMutableDictionary dictionary];
    for (KittyTitlebarTabView *tab in [self tabViews]) existing[@(tab.tab_id)] = tab;
    NSMutableArray<KittyTitlebarTabView*> *new_tabs = [NSMutableArray array];
    NSMutableArray<KittyTitlebarTabView*> *ordered = [NSMutableArray array];
    BOOL active_changed = NO;
    for (size_t i = 0; i < count; i++) {
        KittyTitlebarTabView *tab = existing[@(infos[i].tab_id)];
        if (!tab) {
            tab = [[KittyTitlebarTabView alloc] initWithFrame:NSZeroRect];
            tab.tab_id = infos[i].tab_id;
            [self addSubview:tab];
            [new_tabs addObject:tab];
            [tab release];
        } else {
            [existing removeObjectForKey:@(infos[i].tab_id)];
        }
        tab.os_window_id = self.os_window_id;
        if (tab.is_active != infos[i].is_active) active_changed = YES;
        tab.is_active = infos[i].is_active;
        tab.needs_attention = infos[i].needs_attention;
        tab.fg_rgb = infos[i].fg;
        tab.bg_rgb = infos[i].bg;
        NSString *title = infos[i].title ? @(infos[i].title) : @"";
        if (![tab.titleField.stringValue isEqualToString:title]) tab.titleField.stringValue = title;
        [tab applyColorsAnimated:active_changed && tab.is_active];
        [ordered addObject:tab];
    }
    // re-order subviews only when the tab order actually changed, as
    // removing/re-adding views invalidates their tracking areas
    if (![[self tabViews] isEqualToArray:ordered]) {
        for (KittyTitlebarTabView *tab in ordered) {
            [tab retain];
            [tab removeFromSuperviewWithoutNeedingDisplay];
            [self addSubview:tab];
            [tab release];
        }
    }
    // removed tabs: fade out then remove
    for (NSNumber *key in existing) {
        KittyTitlebarTabView *tab = existing[key];
        tab.marked_for_removal = YES;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = kTabAnimationDuration;
            [[tab animator] setAlphaValue:0.0];
        } completionHandler:^{
            [tab removeFromSuperview];
        }];
    }
    [self layoutTabsAnimated:(new_tabs.count > 0 || existing.count > 0) initialLayoutForNewTabs:new_tabs];
    // re-sync hover state with the actual mouse position, lost mouseExited
    // events would otherwise leave tabs stuck in the hovered state
    NSPoint mouse = [self.window mouseLocationOutsideOfEventStream];
    for (KittyTitlebarTabView *tab in ordered) {
        NSPoint p = [tab convertPoint:mouse fromView:nil];
        BOOL h = NSPointInRect(p, tab.bounds);
        if (h != tab.hovered) {
            tab.hovered = h;
            if (!h) tab.close_hovered = NO;
            [tab applyColorsAnimated:NO];
        }
    }
}

@end

static KittyTitlebarTabBarView*
titlebar_tab_bar_view_for_window(NSWindow *window, bool create) {
    NSButton *close_button = [window standardWindowButton:NSWindowCloseButton];
    NSView *titlebarView = close_button ? close_button.superview : nil;
    if (!titlebarView) return nil;
    for (NSView *v in titlebarView.subviews) {
        if ([v.identifier isEqualToString:TITLEBAR_TABS_IDENTIFIER]) return (KittyTitlebarTabBarView*)v;
    }
    if (!create) return nil;
    KittyTitlebarTabBarView *bar = [[KittyTitlebarTabBarView alloc] initWithFrame:NSZeroRect];
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [titlebarView addSubview:bar];
    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor constraintEqualToAnchor:titlebarView.topAnchor],
        [bar.bottomAnchor constraintEqualToAnchor:titlebarView.bottomAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:titlebarView.leadingAnchor constant:kTabBarLeftMargin],
        [bar.trailingAnchor constraintEqualToAnchor:titlebarView.trailingAnchor constant:-8],
    ]];
    [bar release];
    window.titleVisibility = NSWindowTitleHidden;
    return bar;
}

void
cocoa_update_titlebar_tabs(void *w, unsigned long long os_window_id, const TitlebarTabInfo *tabs, size_t count) { @autoreleasepool {
    NSWindow *window = (NSWindow*)w;
    if (!window) return;
    KittyTitlebarTabBarView *bar = titlebar_tab_bar_view_for_window(window, count > 0);
    if (!bar) return;
    if (count == 0) {
        window.titleVisibility = NSWindowTitleVisible;
        if (@available(macOS 11.0, *)) window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleAutomatic;
        [bar removeFromSuperview];
        return;
    }
    bar.os_window_id = os_window_id;
    window.titleVisibility = NSWindowTitleHidden;
    if (@available(macOS 11.0, *)) window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    [bar updateWithTabs:tabs count:count];
}}

// }}}

// Dock badge {{{

static bool dock_badge_is_set = false;

void
cocoa_set_dock_badge(const char *label) {
    @autoreleasepool {
        NSDockTile *dockTile = [NSApp dockTile];
        [dockTile setBadgeLabel:label ? @(label) : nil];
        [dockTile display];
        dock_badge_is_set = (label != NULL);
    }
}

void
cocoa_clear_dock_badge_if_set(void) {
    if (dock_badge_is_set) cocoa_set_dock_badge(NULL);
}

// }}}

static PyObject*
cocoa_is_secure_input_enabled(PyObject *self UNUSED, PyObject *args UNUSED) {
    SecureKeyboardEntryController *k = [SecureKeyboardEntryController sharedInstance];
    return Py_NewRef(k.isDesired ? Py_True : Py_False);
}

static PyObject*
cocoa_get_machine_id(PyObject *self UNUSED, PyObject *args UNUSED) {
    static char ans[1024] = {0};
    static bool done = false;
    if (!done) {
        done = true;
        CFMutableDictionaryRef matching = IOServiceMatching("IOPlatformExpertDevice");
        // Get the matching service
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
        if (service) {
            CFTypeRef uuid = IORegistryEntryCreateCFProperty(service, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
            if (uuid) {
                // Transfer ownership to NSString using ARC __bridge_transfer
                NSString *s = (NSString*)uuid;
                [s getCString:ans maxLength:sizeof(ans) encoding:NSUTF8StringEncoding];
            }
            // Release the I/O object
            IOObjectRelease(service);
        }
    }
    return PyUnicode_FromString(ans);
}

static PyMethodDef module_methods[] = {
    {"cocoa_play_system_sound_by_id_async", play_system_sound_by_id_async, METH_O, ""},
    {"cocoa_get_lang", (PyCFunction)cocoa_get_lang, METH_NOARGS, ""},
    {"cocoa_get_machine_id", (PyCFunction)cocoa_get_machine_id, METH_NOARGS, ""},
    {"cocoa_is_secure_input_enabled", (PyCFunction)cocoa_is_secure_input_enabled, METH_NOARGS, ""},
    {"cocoa_set_global_shortcut", (PyCFunction)cocoa_set_global_shortcut, METH_VARARGS, ""},
    {"cocoa_send_notification", (PyCFunction)(void(*)(void))cocoa_send_notification, METH_VARARGS | METH_KEYWORDS, ""},
    {"cocoa_remove_delivered_notification", (PyCFunction)cocoa_remove_delivered_notification, METH_O, ""},
    {"cocoa_live_delivered_notifications", (PyCFunction)cocoa_live_delivered_notifications, METH_NOARGS, ""},
    {"cocoa_set_notification_activated_callback", (PyCFunction)set_notification_activated_callback, METH_O, ""},
    {"cocoa_set_url_handler", (PyCFunction)cocoa_set_url_handler, METH_VARARGS, ""},
    {"cocoa_set_app_icon", (PyCFunction)cocoa_set_app_icon, METH_VARARGS, ""},
    {"cocoa_set_dock_icon", (PyCFunction)cocoa_set_dock_icon, METH_VARARGS, ""},
    {"cocoa_show_progress_bar_on_dock_icon", (PyCFunction)cocoa_show_progress_bar_on_dock_icon, METH_VARARGS, ""},
    {"cocoa_bundle_image_as_png", (PyCFunction)(void(*)(void))bundle_image_as_png, METH_VARARGS | METH_KEYWORDS, ""},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

bool
init_cocoa(PyObject *module) {
    cocoa_clear_global_shortcuts();
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    register_at_exit_cleanup_func(COCOA_CLEANUP_FUNC, cleanup);
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidBecomeActiveNotification
        object:nil
        queue:[NSOperationQueue mainQueue]
        usingBlock:^(NSNotification *note UNUSED) {
            cocoa_set_dock_badge(NULL);
        }];
    return true;
}
