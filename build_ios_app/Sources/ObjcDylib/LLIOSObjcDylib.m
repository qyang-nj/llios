#include "LLIOSObjcDylib.h"

@implementation LLIOSObjcDylib
- (NSString *)message:(NSString *)name; {
    NSString *msg = [NSString stringWithFormat:@"Hello, %@!", name];
    return msg;
}
@end
