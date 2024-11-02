// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "CocoaCompatibility.h"

#import "PrefsWindow.h"

@implementation PrefsWindow

- (void)awakeFromNib
{
    [super awakeFromNib];

    self.toolbarStyle = NSWindowToolbarStylePreference;
}

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53) //esc key
    {
        [self close];
    }
    else
    {
        [super keyDown:event];
    }
}

- (void)close
{
    [self makeFirstResponder:nil]; //essentially saves pref changes on window close
    [super close];
}

@end
