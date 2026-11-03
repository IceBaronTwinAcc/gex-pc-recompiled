#import <AppKit/AppKit.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "game_install.h"

int gex_default_game_folder(char *path, size_t size) {
    @autoreleasepool {
        NSBundle *bundle = [NSBundle mainBundle];
        NSString *folder = [[bundle.bundlePath pathExtension] isEqualToString:@"app"] ?
            [bundle.resourcePath stringByAppendingPathComponent:@"Gex"] : @"Gex";
        const char *native = folder.UTF8String;
        if (!native) return -1;
        int n = snprintf(path, size, "%s", native);
        return n < 0 || (size_t)n >= size ? -1 : 0;
    }
}

int gex_pick_game_folder(char *path, size_t size, char *error, size_t error_size) {
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            snprintf(error, error_size, "Cannot initialize the folder picker: %s", SDL_GetError());
            return -1;
        }
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert *notice = [[NSAlert alloc] init];
        notice.messageText = @"Select the Gex installation folder";
        notice.informativeText = @"Select the folder with the installation of the PC version of Gex from GOG.";
        notice.alertStyle = NSAlertStyleInformational;
        [notice addButtonWithTitle:@"Continue"];
        [notice addButtonWithTitle:@"Cancel"];
        NSModalResponse response = [notice runModal];
        [notice release];
        if (response != NSAlertFirstButtonReturn) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return 1;
        }
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Select the Gex installation folder";
        panel.message = @"Select the folder containing GEX.exe. The original game will be copied "
                         "into this application's bundle. Move the app to a writable folder first.";
        panel.prompt = @"Import";
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return 1;
        }
        const char *selected = panel.URL.path.UTF8String;
        if (!selected || strlen(selected) >= size) {
            snprintf(error, error_size, "The selected game folder path is too long");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return -1;
        }
        strcpy(path, selected);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }
}

void gex_setup_error(const char *message) {
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fprintf(stderr, "[setup] cannot display the setup error: %s\n", SDL_GetError());
            return;
        }
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Gex setup";
        alert.informativeText = [NSString stringWithUTF8String:message] ?:
            @"Game import failed. See the application log.";
        alert.alertStyle = NSAlertStyleCritical;
        [alert runModal];
        [alert release];
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}
