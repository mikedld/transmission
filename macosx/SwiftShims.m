// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

/*
 * These are the shims between Apple's ObjC frameworks and Swift, for ObjC->Swift bridging headers.
 * Best kept as a pure ObjC file and not an ObjC++ file.
 */

#import "SwiftShims.h"

long long const swiftNSURLResponseUnknownLength = NSURLResponseUnknownLength;

@implementation ObjC

+ (BOOL)catchException:(NS_NOESCAPE void (^_Nonnull)(void))tryBlock error:(__autoreleasing NSError* _Nullable* _Nonnull)error
{
    @try
    {
        tryBlock();
        return YES;
    }
    @catch (NSException* exception)
    {
        NSDictionary* userInfo = exception.userInfo;
        if (exception.reason && exception.userInfo[NSLocalizedFailureReasonErrorKey] == nil)
        {
            NSMutableDictionary* userInfoCopy = [NSMutableDictionary dictionaryWithDictionary:userInfo ?: @{}];
            userInfoCopy[NSLocalizedFailureReasonErrorKey] = exception.reason;
            userInfo = userInfoCopy;
        }
        *error = [[NSError alloc] initWithDomain:exception.name code:0 userInfo:userInfo];
        return NO;
    }
}

@end
