#import <Cocoa/Cocoa.h>

/**
 * Launch Services шлёт Apple Event «reopen», когда плагин снова делает open().
 * Без NSRunLoop процесс «не отвечает», и macOS показывает диалог.
 */
extern "C" void pianoled_run_app_loop(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp run];
    }
}
