// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Foundation/Foundation.h>

__attribute__((annotate("returns_localized_nsstring"))) static inline NSString* LocalizationNotNeeded(NSString* s)
{
    return s;
}
