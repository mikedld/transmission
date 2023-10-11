// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#import <Transmission-Swift.h>

@interface Transmission (CXX)
@property(nonatomic, class) tr_session* session;
@end
