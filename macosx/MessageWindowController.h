// This file Copyright (c) 2006-2022 Transmission authors and contributors
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

@interface MessageWindowController : NSWindowController
{
    IBOutlet NSTableView* fMessageTable;

    IBOutlet NSPopUpButton* fLevelButton;
    IBOutlet NSButton* fSaveButton;
    IBOutlet NSButton* fClearButton;
    IBOutlet NSSearchField* fFilterField;

    NSMutableArray* fMessages;
    NSMutableArray* fDisplayedMessages;

    NSDictionary* fAttributes;

    NSTimer* fTimer;

    NSLock* fLock;
}

- (void)updateLog:(NSTimer*)timer;

- (void)changeLevel:(id)sender;
- (void)changeFilter:(id)sender;
- (void)clearLog:(id)sender;

- (void)writeToFile:(id)sender;

@end
