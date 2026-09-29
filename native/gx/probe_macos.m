#import <Foundation/Foundation.h>

void petari_probe_disable_window_restoration(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{
            @"ApplePersistenceIgnoreState": @YES,
            @"NSQuitAlwaysKeepsWindows": @NO
        }];
    }
}
