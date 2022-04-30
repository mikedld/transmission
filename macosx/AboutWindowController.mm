// This file Copyright © 2007-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/version.h>

#import "AboutWindowController.h"

@interface AboutWindowController ()

@property(nonatomic) IBOutlet NSTextView* fTextView;
@property(nonatomic) IBOutlet NSTextView* fLicenseView;
@property(nonatomic) IBOutlet NSTextField* fVersionField;
@property(nonatomic) IBOutlet NSTextField* fCopyrightField;
@property(nonatomic) IBOutlet NSButton* fLicenseButton;
@property(nonatomic) IBOutlet NSButton* fLicenseCloseButton;
@property(nonatomic) IBOutlet NSPanel* fLicenseSheet;

@end

@implementation AboutWindowController

AboutWindowController* fAboutBoxInstance = nil;

+ (AboutWindowController*)aboutController
{
    if (!fAboutBoxInstance)
    {
        fAboutBoxInstance = [[self alloc] initWithWindowNibName:@"AboutWindow"];
    }
    return fAboutBoxInstance;
}

- (void)awakeFromNib
{
    self.fVersionField.stringValue = @(LONG_VERSION_STRING);

    self.fCopyrightField.stringValue = [NSBundle.mainBundle localizedStringForKey:@"NSHumanReadableCopyright" value:nil
                                                                            table:@"InfoPlist"];

    [self.fTextView.textStorage
        setAttributedString:[[NSAttributedString alloc] initWithPath:[NSBundle.mainBundle pathForResource:@"Credits" ofType:@"rtf"]
                                                  documentAttributes:nil]];

    //size license button
    CGFloat const oldButtonWidth = NSWidth(self.fLicenseButton.frame);

    self.fLicenseButton.title = NSLocalizedString(@"License", "About window -> license button");
    [self.fLicenseButton sizeToFit];

    NSRect buttonFrame = self.fLicenseButton.frame;
    buttonFrame.size.width += 10.0;
    buttonFrame.origin.x -= NSWidth(buttonFrame) - oldButtonWidth;
    self.fLicenseButton.frame = buttonFrame;
}

- (void)windowDidLoad
{
    [self.window center];
}

- (void)windowWillClose:(id)sender
{
    fAboutBoxInstance = nil;
}

- (IBAction)showLicense:(id)sender
{
    NSString* licenseText = [NSString stringWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"COPYING" ofType:nil]
                                                  usedEncoding:nil
                                                         error:NULL];
    self.fLicenseView.string = licenseText;
    self.fLicenseCloseButton.title = NSLocalizedString(@"OK", "About window -> license close button");

    [self.window beginSheet:self.fLicenseSheet completionHandler:nil];
}

- (IBAction)hideLicense:(id)sender
{
    [self.fLicenseSheet orderOut:nil];
    [NSApp endSheet:self.fLicenseSheet];
}

@end
