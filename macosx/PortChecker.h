// This file Copyright © 2006-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Foundation/Foundation.h>

typedef NS_ENUM(unsigned int, port_status_t) { //
    PORT_STATUS_CHECKING,
    PORT_STATUS_OPEN,
    PORT_STATUS_CLOSED,
    PORT_STATUS_ERROR
};

@interface PortChecker : NSObject

@property(nonatomic, readonly) port_status_t status;

- (instancetype)initForPort:(NSInteger)portNumber delay:(BOOL)delay withDelegate:(id)delegate;
- (void)cancelProbe;

@end
