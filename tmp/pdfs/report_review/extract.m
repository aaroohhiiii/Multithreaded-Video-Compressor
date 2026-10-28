#import <AppKit/AppKit.h>
#import <PDFKit/PDFKit.h>
#import <Foundation/Foundation.h>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 4) {
            fprintf(stderr, "usage: extract INPUT.pdf OUTPUT.txt IMAGE_DIR\n");
            return 2;
        }

        NSString *input = [NSString stringWithUTF8String:argv[1]];
        NSString *textOutput = [NSString stringWithUTF8String:argv[2]];
        NSString *imageDirectory = [NSString stringWithUTF8String:argv[3]];
        PDFDocument *document = [[PDFDocument alloc]
            initWithURL:[NSURL fileURLWithPath:input]];
        if (document == nil) {
            fprintf(stderr, "could not open PDF\n");
            return 1;
        }

        NSError *error = nil;
        [[NSFileManager defaultManager]
            createDirectoryAtPath:imageDirectory
            withIntermediateDirectories:YES
            attributes:nil
            error:&error];
        if (error != nil) {
            fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
            return 1;
        }

        NSMutableString *completeText = [NSMutableString string];
        for (NSInteger pageIndex = 0; pageIndex < document.pageCount; ++pageIndex) {
            PDFPage *page = [document pageAtIndex:pageIndex];
            [completeText appendFormat:@"\n===== PAGE %ld =====\n", pageIndex + 1];
            [completeText appendString:page.string ?: @"[NO EXTRACTABLE TEXT]"];
            [completeText appendString:@"\n"];

            NSImage *image = [page thumbnailOfSize:NSMakeSize(1200, 1600)
                                             forBox:kPDFDisplayBoxMediaBox];
            NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc]
                initWithData:image.TIFFRepresentation];
            NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                                properties:@{}];
            NSString *name = [NSString stringWithFormat:@"page-%02ld.png", pageIndex + 1];
            NSString *path = [imageDirectory stringByAppendingPathComponent:name];
            if (![png writeToFile:path atomically:YES]) {
                fprintf(stderr, "could not render page %ld\n", pageIndex + 1);
                return 1;
            }
        }

        if (![completeText writeToFile:textOutput
                             atomically:YES
                               encoding:NSUTF8StringEncoding
                                  error:&error]) {
            fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        printf("pages=%ld\n", document.pageCount);
    }
    return 0;
}
