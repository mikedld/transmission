// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "objc_log.h"
#import <libtransmission/log.h>

int C_TR_LOG_MAX_QUEUE_LENGTH = TR_LOG_MAX_QUEUE_LENGTH;

@interface TRLogMessage ()
@property(nonatomic) tr_log_message* log_message;
@end
@implementation TRLogMessage
- (instancetype)initWithLogMessage:(tr_log_message*)log_message
{
    self = super.init;
    _log_message = log_message;
    return self;
}
- (enum c_tr_log_level)level
{
    return (c_tr_log_level)_log_message->level;
}
- (NSString*)file
{
    auto const file_string = std::string{ _log_message->file };
    return @(file_string.c_str());
}
- (long)line
{
    return _log_message->line;
}
- (time_t)when
{
    return _log_message->when;
}
- (char const*)name
{
    return _log_message->name.c_str();
}
- (char const*)message
{
    return _log_message->message.c_str();
}
- (TRLogMessage*)next
{
    tr_log_message* next;
    if ((next = _log_message->next) == NULL)
    {
        return nil;
    }
    return [[TRLogMessage alloc] initWithLogMessage:next];
}
@end

void c_tr_logSetQueueEnabled(bool is_enabled)
{
    tr_logSetQueueEnabled(is_enabled);
}
TRLogMessage* c_tr_logGetQueue()
{
    tr_log_message* queue;
    if ((queue = tr_logGetQueue()) == NULL)
    {
        return nil;
    }
    return [[TRLogMessage alloc] initWithLogMessage:queue];
}
void c_tr_logFreeQueue(TRLogMessage* freeme)
{
    tr_logFreeQueue(freeme.log_message);
}
void c_tr_logSetLevel(enum c_tr_log_level level)
{
    tr_logSetLevel((tr_log_level)level);
}
enum c_tr_log_level c_tr_logGetLevel()
{
    return (c_tr_log_level)tr_logGetLevel();
}
bool c_tr_logLevelIsActive(enum c_tr_log_level level)
{
    return tr_logLevelIsActive((tr_log_level)level);
}
