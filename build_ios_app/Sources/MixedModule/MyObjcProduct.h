@import Foundation;

@class MySwiftMaterial;

// MyObjcProduct depends on a Swift class SwiftMaterial
// MyObjcProduct is depended by a Swift class SwiftProducer
@interface MyObjcProduct : NSObject
@property (strong, readonly) NSString *name;
@property (strong, readwrite) MySwiftMaterial *material;

- (instancetype)initWithName:(NSString *)name;
- (NSString *)materialType;
@end
