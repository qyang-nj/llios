@import Foundation;

// MyObjcProduct depends on a Swift class SwiftMaterial
// MyObjcProduct is depended by a Swift class SwiftProducer
@interface MyObjcProduct : NSObject

@property (strong, readonly) NSString *name;

- (instancetype)initWithName:(NSString *)name;

- (NSString *)materialType;

@end
