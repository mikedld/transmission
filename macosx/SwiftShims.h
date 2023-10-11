// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

/*
 * These are the shims between Apple's ObjC frameworks and Swift, for ObjC->Swift bridging headers.
 */

#import <Foundation/Foundation.h>

extern long long const swiftNSURLResponseUnknownLength;

@interface ObjC : NSObject

+ (BOOL)catchException:(NS_NOESCAPE void (^_Nonnull)(void))tryBlock error:(__autoreleasing NSError* _Nullable* _Nonnull)error;

@end
