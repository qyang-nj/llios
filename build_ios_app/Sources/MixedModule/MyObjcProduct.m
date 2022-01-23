#import "MixedModule-Swift.h"
#import "MyObjcProduct.h"

@interface MyObjcProduct()
@property (strong, readwrite) MySwiftMaterial *material;
@end


@implementation MyObjcProduct
- (instancetype)initWithName:(NSString *)name {
    self = [super init];
    if (self) {
        _name = name;
        _material = [[MySwiftMaterial alloc] init];
    }
    return self;
}

- (NSString *)materialType {
    return [self.material type];
}
@end
