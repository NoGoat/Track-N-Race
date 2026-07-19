#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^TNRSessionUpdateHandler)(NSString* circuit, NSString* session);
typedef void (^TNRRecordingUpdateHandler)(NSString* status, NSString* error);

@interface TNRAttribution : NSObject

@property(nonatomic, copy, readonly) NSString* name;
@property(nonatomic, copy, readonly) NSString* version;
@property(nonatomic, copy, readonly) NSString* licenseName;
@property(nonatomic, copy, readonly) NSString* copyrightText;
@property(nonatomic, copy, readonly) NSString* website;
@property(nonatomic, copy, readonly) NSString* licenseText;

@end

@interface TNRRecorderBridge : NSObject

@property(nonatomic, copy, readonly) NSString* outputFolder;
@property(nonatomic, copy, readonly) NSString* bindAddress;
@property(nonatomic, readonly) NSInteger port;
@property(nonatomic, readonly) NSInteger protocolIndex;
@property(nonatomic, copy, nullable) TNRSessionUpdateHandler sessionUpdateHandler;
@property(nonatomic, copy, nullable) TNRRecordingUpdateHandler recordingUpdateHandler;

- (instancetype)initWithDefaults:(NSUserDefaults*)defaults NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Returns nil on success or a user-facing error message on failure.
- (nullable NSString*)start;
- (nullable NSString*)selectOutputFolder:(NSString*)folder;
- (nullable NSString*)applyNetworkAddress:(NSString*)address port:(NSInteger)port;
- (void)setProtocolIndex:(NSInteger)index;
- (NSArray<TNRAttribution*>*)attributions;

@end

NS_ASSUME_NONNULL_END
