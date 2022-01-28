#import "MixedModule-Swift.h"
#import "MyObjcProduct.h"

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
