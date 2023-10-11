// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "objc.h"

NS_ASSUME_NONNULL_BEGIN
TR_EXTERN_C_BEGIN

enum c_tr_log_level
{
    C_TR_LOG_OFF,
    C_TR_LOG_CRITICAL,
    C_TR_LOG_ERROR,
    C_TR_LOG_WARN,
    C_TR_LOG_INFO,
    C_TR_LOG_DEBUG,
    C_TR_LOG_TRACE
};

@interface TRLogMessage : NSObject
@property(nonatomic, readonly) enum c_tr_log_level level;
@property(nonatomic, readonly) NSString* file;
@property(nonatomic, readonly) long line;
@property(nonatomic, readonly) time_t when;
@property(nonatomic, readonly) char const* name;
@property(nonatomic, readonly) char const* message;
- (TRLogMessage* _Nullable)next;
@end

extern int C_TR_LOG_MAX_QUEUE_LENGTH;

void c_tr_logSetQueueEnabled(bool is_enabled);
TRLogMessage* _Nullable c_tr_logGetQueue();
void c_tr_logFreeQueue(TRLogMessage* freeme);
void c_tr_logSetLevel(enum c_tr_log_level level);
enum c_tr_log_level c_tr_logGetLevel();
bool c_tr_logLevelIsActive(enum c_tr_log_level level);

TR_EXTERN_C_END
NS_ASSUME_NONNULL_END
