#include "LLIOSObjcDylib.h"

@implementation LLIOSObjcDylib
- (void)sayHello:(NSString *)name; {
    NSLog(@"[ObjcDylib] Hello, %@!", name);
}
@end
