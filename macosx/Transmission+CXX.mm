// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "Transmission+CXX.h"
#import <Transmission-Swift.h>

@implementation Transmission (CXX)
+ (void)setSession:(tr_session*)session
{
    self.cSession = (c_tr_session*)session;
}
+ (tr_session*)session
{
    return (tr_session*)self.cSession;
}
@end
