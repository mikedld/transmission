// This file Copyright © 2006-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "PortChecker.h"

#define CHECKER_URL(port) [NSString stringWithFormat:@"https://portcheck.transmissionbt.com/%ld", port]
#define CHECK_FIRE 3.0

@interface PortChecker (Private)

- (void)startProbe:(NSTimer*)timer;

- (void)callBackWithStatus:(port_status_t)status;

@end

@implementation PortChecker

- (instancetype)initForPort:(NSInteger)portNumber delay:(BOOL)delay withDelegate:(id)delegate
{
    if ((self = [super init]))
    {
        fDelegate = delegate;

        fStatus = PORT_STATUS_CHECKING;

        fTimer = [NSTimer scheduledTimerWithTimeInterval:CHECK_FIRE target:self selector:@selector(startProbe:)
                                                userInfo:@(portNumber)
                                                 repeats:NO];
        if (!delay)
        {
            [fTimer fire];
        }
    }

    return self;
}

- (void)dealloc
{
    [fTimer invalidate];
}

- (port_status_t)status
{
    return fStatus;
}

- (void)cancelProbe
{
    [fTimer invalidate];
    fTimer = nil;

    [fConnection cancel];
}

- (void)connection:(NSURLConnection*)connection didReceiveResponse:(NSURLResponse*)response
{
    fPortProbeData.length = 0;
}

- (void)connection:(NSURLConnection*)connection didReceiveData:(NSData*)data
{
    [fPortProbeData appendData:data];
}

- (void)connection:(NSURLConnection*)connection didFailWithError:(NSError*)error
{
    NSLog(@"Unable to get port status: connection failed (%@)", error.localizedDescription);
    [self callBackWithStatus:PORT_STATUS_ERROR];
}

- (void)connectionDidFinishLoading:(NSURLConnection*)connection
{
    NSString* probeString = [[NSString alloc] initWithData:fPortProbeData encoding:NSUTF8StringEncoding];
    fPortProbeData = nil;

    if (probeString)
    {
        if ([probeString isEqualToString:@"1"])
        {
            [self callBackWithStatus:PORT_STATUS_OPEN];
        }
        else if ([probeString isEqualToString:@"0"])
        {
            [self callBackWithStatus:PORT_STATUS_CLOSED];
        }
        else
        {
            NSLog(@"Unable to get port status: invalid response (%@)", probeString);
            [self callBackWithStatus:PORT_STATUS_ERROR];
        }
    }
    else
    {
        NSLog(@"Unable to get port status: invalid data received");
        [self callBackWithStatus:PORT_STATUS_ERROR];
    }
}

@end

@implementation PortChecker (Private)

- (void)startProbe:(NSTimer*)timer
{
    fTimer = nil;

    NSURLRequest* portProbeRequest = [NSURLRequest requestWithURL:[NSURL URLWithString:CHECKER_URL([[timer userInfo] integerValue])]
                                                      cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData
                                                  timeoutInterval:15.0];

    if ((fConnection = [[NSURLConnection alloc] initWithRequest:portProbeRequest delegate:self]))
    {
        fPortProbeData = [[NSMutableData alloc] init];
    }
    else
    {
        NSLog(@"Unable to get port status: failed to initiate connection");
        [self callBackWithStatus:PORT_STATUS_ERROR];
    }
}

- (void)callBackWithStatus:(port_status_t)status
{
    fStatus = status;

    if (fDelegate && [fDelegate respondsToSelector:@selector(portCheckerDidFinishProbing:)])
    {
        [fDelegate performSelectorOnMainThread:@selector(portCheckerDidFinishProbing:) withObject:self waitUntilDone:NO];
    }
}

@end
