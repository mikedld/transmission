// This file Copyright © 2009-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "NSApplicationAdditions.h"

@implementation NSApplication (NSApplicationAdditions)

- (BOOL)isOnMojaveOrBetter
{
    return floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_13;
}

- (BOOL)isDarkMode
{
    if (@available(macOS 10.14, *))
    {
        return [self.effectiveAppearance.name isEqualToString:NSAppearanceNameDarkAqua];
    }
    else
    {
        return NO;
    }
}

@end
