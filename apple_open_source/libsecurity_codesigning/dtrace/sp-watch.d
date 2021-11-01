#!/usr/sbin/dtrace -qs
#
#
#
self unsigned char *cdhash;

syspolicy*:::assess-*
{
	self->cdhash = 0;
}

self string type;
syspolicy*:::assess-outcome-* / arg1 == 1 / { type = "execute"; }
syspolicy*:::assess-outcome-* / arg1 == 2 / { type = "install"; }

syspolicy*:::assess-outcome-accept
{
	printf("accept %s %s;%s", self->type, copyinstr(arg0), copyinstr(arg1));
	self->cdhash = copyin(arg2, 20);
}

syspolicy*:::assess-outcome-deny
{
	printf("deny %s %s;%s", self->type, copyinstr(arg0), copyinstr(arg1));
	self->cdhash = copyin(arg2, 20);
}

syspolicy*:::assess-outcome-default
{
	printf("default %s %s;%s", self->type, copyinstr(arg0), copyinstr(arg1));
	self->cdhash = copyin(arg2, 20);
}

syspolicy*:::assess-outcome-unsigned
{
	printf("unsigned %s %s;", self->type, copyinstr(arg0));
}

syspolicy*:::assess-*
/ self->cdhash /
{
	printf(";%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x%02.2x",
		self->cdhash[0], self->cdhash[1], self->cdhash[2], self->cdhash[3], self->cdhash[4],
		self->cdhash[5], self->cdhash[6], self->cdhash[7], self->cdhash[8], self->cdhash[9],
		self->cdhash[10], self->cdhash[11], self->cdhash[12], self->cdhash[13], self->cdhash[14],
		self->cdhash[15], self->cdhash[16], self->cdhash[17], self->cdhash[18], self->cdhash[19]);
}

syspolicy*:::assess-*
{
	printf("\n");
}
