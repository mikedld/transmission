//
//  PreviewProvider.m
//  QuickLookExtension
//
//  Created by Dzmitry Neviadomski on 18.10.24.
//  Copyright © 2024 The Transmission Project. All rights reserved.
//

#import "PreviewProvider.h"

@implementation PreviewProvider

/*

 Use a QLPreviewProvider to provide data-based previews.
 
 To set up your extension as a data-based preview extension:

 - Modify the extension's Info.plist by setting
   <key>QLIsDataBasedPreview</key>
   <true/>
 
 - Add the supported content types to QLSupportedContentTypes array in the extension's Info.plist.

 - Change the NSExtensionPrincipalClass to this class.
   e.g.
   <key>NSExtensionPrincipalClass</key>
   <string>PreviewProvider</string>
 
 - Implement providePreviewForFileRequest:completionHandler:
 
 */

- (void)providePreviewForFileRequest:(QLFilePreviewRequest *)request completionHandler:(void (^)(QLPreviewReply * _Nullable reply, NSError * _Nullable error))handler
{
    //You can create a QLPreviewReply in several ways, depending on the format of the data you want to return.
    //To return NSData of a supported content type:
    
    UTType* contentType = UTTypeUTF8PlainText; //replace with your data type
    
    QLPreviewReply* reply = [[QLPreviewReply alloc] initWithDataOfContentType:contentType contentSize:CGSizeMake(800, 800) dataCreationBlock:^NSData * _Nullable(QLPreviewReply * _Nonnull replyToUpdate, NSError *__autoreleasing  _Nullable * _Nullable error) {

        //setting the stringEncoding for text and html data is optional and defaults to NSUTF8StringEncoding
        replyToUpdate.stringEncoding = NSUTF8StringEncoding;

        NSString* string = @"Hello, World!";

        //initialize your data here
        return [string dataUsingEncoding:NSUTF8StringEncoding];
    }];
    
    //You can also create a QLPreviewReply with a fileURL of a supported file type, by drawing directly into a bitmap context, or by providing a PDFDocument.
    
    handler(reply, nil);
}

@end

