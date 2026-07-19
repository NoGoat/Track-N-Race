#import "TNRRecorderBridge.h"

#include "Attributions.h"
#include "MinimalController.h"

#import <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

NSString* nsString(std::string_view value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data()
                                                length:value.size()
                                              encoding:NSUTF8StringEncoding];
    return result ?: @"";
}

std::string cppString(NSString* value) {
    const char* bytes = value.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}

tnrp::Override protocolFromIndex(NSInteger index) {
    switch (index) {
        case 1: return tnrp::Override::F1_24;
        case 2: return tnrp::Override::F1_25;
        case 3: return tnrp::Override::F1_26;
        default: return tnrp::Override::Auto;
    }
}

NSInteger indexFromProtocol(tnrp::Override protocol) {
    switch (protocol) {
        case tnrp::Override::F1_24: return 1;
        case tnrp::Override::F1_25: return 2;
        case tnrp::Override::F1_26: return 3;
        default: return 0;
    }
}

} // namespace

@interface TNRAttribution ()

- (instancetype)initWithAttribution:(const Attribution&)attribution;

@property(nonatomic, copy, readwrite) NSString* name;
@property(nonatomic, copy, readwrite) NSString* version;
@property(nonatomic, copy, readwrite) NSString* licenseName;
@property(nonatomic, copy, readwrite) NSString* copyrightText;
@property(nonatomic, copy, readwrite) NSString* website;
@property(nonatomic, copy, readwrite) NSString* licenseText;

@end

@implementation TNRAttribution

- (instancetype)initWithAttribution:(const Attribution&)attribution {
    self = [super init];
    if (self) {
        _name = nsString(attribution.name);
        _version = nsString(attribution.version);
        _licenseName = nsString(attribution.license);
        _copyrightText = nsString(attribution.copyright);
        _website = nsString(attribution.website);
        _licenseText = nsString(attribution.licenseText);
    }
    return self;
}

@end

@interface TNRRecorderBridge () {
    NSUserDefaults* _defaults;
    std::unique_ptr<MinimalController> _controller;
}

@property(nonatomic, copy, readwrite) NSString* outputFolder;
@property(nonatomic, copy, readwrite) NSString* bindAddress;
@property(nonatomic, readwrite) NSInteger port;
@property(nonatomic, readwrite) NSInteger protocolIndex;

@end

@implementation TNRRecorderBridge

- (instancetype)initWithDefaults:(NSUserDefaults*)defaults {
    self = [super init];
    if (!self) return nil;

    _defaults = defaults;
    NSString* storedFolder = [defaults stringForKey:@"output-folder"];
    NSString* storedAddress = [defaults stringForKey:@"bind-address"];
    NSInteger storedPort = [defaults integerForKey:@"port"];
    NSString* storedProtocol = [defaults stringForKey:@"protocol"];

    _outputFolder = storedFolder ?: @"";
    _bindAddress = storedAddress.length > 0 ? storedAddress : @"0.0.0.0";
    _port = storedPort >= 1 && storedPort <= 65535 ? storedPort : 20777;
    const tnrp::Override protocol = tnrp::overrideFromString(
        storedProtocol ? cppString(storedProtocol) : std::string("auto"));
    _protocolIndex = indexFromProtocol(protocol);

    AppSettings settings;
    settings.outputFolder = cppString(_outputFolder);
    settings.bindAddress = cppString(_bindAddress);
    settings.port = static_cast<uint16_t>(_port);
    settings.protocol = protocol;
    _controller = std::make_unique<MinimalController>(std::move(settings));

    __weak TNRRecorderBridge* weakSelf = self;
    _controller->setSessionCallback(
        [weakSelf](std::string circuit, std::string session) {
            NSString* circuitText = nsString(circuit);
            NSString* sessionText = nsString(session);
            dispatch_async(dispatch_get_main_queue(), ^{
                TNRRecorderBridge* strongSelf = weakSelf;
                TNRSessionUpdateHandler handler = strongSelf.sessionUpdateHandler;
                if (handler) handler(circuitText, sessionText);
            });
        });
    _controller->setRecordingCallback(
        [weakSelf](std::string status, std::string error) {
            NSString* statusText = nsString(status);
            NSString* errorText = error.empty() ? @"None" : nsString(error);
            dispatch_async(dispatch_get_main_queue(), ^{
                TNRRecorderBridge* strongSelf = weakSelf;
                TNRRecordingUpdateHandler handler = strongSelf.recordingUpdateHandler;
                if (handler) handler(statusText, errorText);
            });
        });
    return self;
}

- (void)dealloc {
    if (_controller) {
        _controller->setSessionCallback({});
        _controller->setRecordingCallback({});
        _controller.reset();
    }
}

- (nullable NSString*)start {
    std::string error;
    return _controller->start(error) ? nil : nsString(error);
}

- (nullable NSString*)selectOutputFolder:(NSString*)folder {
    std::string error;
    if (!_controller->setOutputFolder(cppString(folder), error)) return nsString(error);

    self.outputFolder = folder;
    [_defaults setObject:folder forKey:@"output-folder"];
    return nil;
}

- (nullable NSString*)applyNetworkAddress:(NSString*)address port:(NSInteger)port {
    if (port < 1 || port > 65535) {
        return @"The UDP port must be between 1 and 65535.";
    }

    std::string error;
    if (!_controller->applyNetwork(cppString(address), static_cast<uint16_t>(port), error)) {
        return nsString(error);
    }

    self.bindAddress = address;
    self.port = port;
    [_defaults setObject:address forKey:@"bind-address"];
    [_defaults setInteger:port forKey:@"port"];
    return nil;
}

- (void)setProtocolIndex:(NSInteger)index {
    const NSInteger safeIndex = index >= 0 && index <= 3 ? index : 0;
    const tnrp::Override protocol = protocolFromIndex(safeIndex);
    _controller->setProtocol(protocol);
    self.protocolIndex = safeIndex;
    [_defaults setObject:nsString(tnrp::toString(protocol)) forKey:@"protocol"];
}

- (NSArray<TNRAttribution*>*)attributions {
    NSMutableArray<TNRAttribution*>* result = [NSMutableArray array];
    for (const Attribution& item : minimalAppAttributions()) {
        [result addObject:[[TNRAttribution alloc] initWithAttribution:item]];
    }
    return result;
}

@end
