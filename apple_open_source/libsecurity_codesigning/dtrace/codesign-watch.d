#!/usr/sbin/dtrace -q -s
/*
 * Demonstration D script for watching Code Signing activity in the system
 *
 * As presented, this script will record and report all Code Signing activity
 * in one process (argument=pid), or all processes (argument='*').
 * You are encouraged to modify it as you will. (A good start is to comment out
 * the print statements you don't like to see.)
 */
typedef uint64_t DTHandle;		/* generic API handle (NOT a pointer) */
typedef uint8_t Hash[20];		/* SHA-1 */

typedef struct {				/* from implementation */
	uint32_t cputype;
	uint32_t cpusubtype;
	off_t offset;
	uint8_t fileOnly;
} DiskRepContext;


/*
 * Local variables used for suitable casting (only)
 */
self uint8_t *hash;


/*
 * Startup (this may take a while)
 */
:::BEGIN
{
	printf("Ready...\n");
}


/*
 * Finishing (add statistics tracers here)
 */
:::END
{
}


/*
 * Track kernel-related objects.
 * Each process has their own, and they're usually created very early.
 */
struct {
	DTHandle rep;				/* DiskRep */
	DTHandle staticCode;		/* static code */
	DTHandle code;				/* dynamic code */
} kernel[pid_t];


/*
 * Track DiskRep objects.
 * DiskReps are drivers for on-disk formats. Beyond their natural concerns,
 * they also carry the path information for StaticCode objects.
 */
typedef struct {
	DTHandle me;				/* own handle, if valid */
	string path;				/* canonical path */
	string type;				/* type string */
	DiskRepContext ctx;			/* construction context, if any */
	DTHandle sub;				/* sub-DiskRep if any */
} DiskRep;
DiskRep rep[DTHandle];			/* all the DiskReps we've seen */

self uint64_t ctx;				/* passes construction context, NULL if none */

codesign$1:::diskrep-create-*	/* preset none */
{ self->ctx = 0; }

codesign$1:::diskrep-create-kernel
{
	rep[arg0].me = kernel[pid].rep = arg0;
	rep[arg0].path = "(kernel)";
	rep[arg0].type = "kernel";
	printf("%8u %s[%d]%s(%p,KERNEL)\n",
		timestamp, execname, pid, probename, arg0);
}

codesign$1:::diskrep-create-macho
{
	rep[arg0].me = arg0;
	rep[arg0].path = copyinstr(arg1);
	rep[arg0].type = "macho";
	self->ctx = arg2;
	printf("%8u %s[%d]%s(%p,%s)\n",
		timestamp, execname, pid, probename, arg0, rep[arg0].path);
}

codesign$1:::diskrep-create-bundle-path
{
	rep[arg0].me = arg0;
	rep[arg0].path = copyinstr(arg1);
	rep[arg0].type = "bundle";
	self->ctx = arg2;
	rep[arg0].sub = arg3;
	printf("%8u %s[%d]%s(%p,%s,%p)\n",
		timestamp, execname, pid, probename,
		arg0, rep[arg0].path, rep[arg0].sub);
}

codesign$1:::diskrep-create-bundle-ref
{
	rep[arg0].me = arg0;
	rep[arg0].path = "(from ref)";
	rep[arg0].type = "bundle";
	self->ctx = arg2;
	rep[arg0].sub = arg3;
	printf("%8u %s[%d]%s(%p,%s,%p)\n",
		timestamp, execname, pid, probename,
		arg0, rep[arg0].path, rep[arg0].sub);
}

codesign$1:::diskrep-create-file
{
	rep[arg0].me = arg0;
	rep[arg0].path = copyinstr(arg1);
	rep[arg0].type = "file";
	printf("%8u %s[%d]%s(%p,%s)\n",
		timestamp, execname, pid, probename, arg0, rep[arg0].path);
}

self DiskRepContext *ctxp;

codesign$1:::diskrep-create-*
/ self->ctx /
{
	self->ctxp = (DiskRepContext *)copyin(self->ctx, sizeof(DiskRepContext));
	rep[arg0].ctx = *self->ctxp;
	printf("%8u %s[%d] ...context: arch=(0x%x,0x%x) offset=0x%x file=%d\n",
		timestamp, execname, pid,
		self->ctxp->cputype, self->ctxp->cpusubtype,
		self->ctxp->offset, self->ctxp->fileOnly);
}

codesign$1:::diskrep-destroy
{
	printf("%8u %s[%d]%s(%p,%s)\n",
		timestamp, execname, pid, probename, arg0, rep[arg0].path);
	rep[arg0].me = 0;
}


/*
 * Track Code Signing API objects
 */
typedef struct {
	DTHandle me;
	DTHandle host;
	DTHandle staticCode;	/* lazily acquired */
	uint8_t *hash;			/* dynamic hash from identify() */
} Code;
Code code[DTHandle];

typedef struct {
	DTHandle me;
	DTHandle rep;
	uint8_t *hash;			/* static hash from ...::cdHash() */
} StaticCode;
StaticCode staticCode[DTHandle];


codesign$1:::static-create
/ arg1 == kernel[pid].rep /
{
	staticCode[arg0].me = kernel[pid].staticCode = arg0;
	staticCode[arg0].rep = arg1;
	printf("%8u %s[%d]%s(%p=KERNEL[%p])\n",
		timestamp, execname, pid, probename, arg0, arg1);
}

codesign$1:::static-create
/ arg1 != kernel[pid].rep /
{
	staticCode[arg0].me = arg0;
	staticCode[arg0].rep = arg1;
	printf("%8u %s[%d]%s(%p,%s[%p])\n",
		timestamp, execname, pid, probename, arg0, rep[arg1].path, arg1);
}

codesign$1:::dynamic-create
/ arg1 == 0 /
{
	code[arg0].me = kernel[pid].code = arg0;
	printf("%8u %s[%d]%s(%p=KERNEL)\n",
		timestamp, execname, pid, probename, arg0);
}

codesign$1:::dynamic-create
/ arg1 == kernel[pid].code /
{
	code[arg0].me = arg0;
	printf("%8u %s[%d]%s(%p,<KERNEL>)\n",
		timestamp, execname, pid, probename, arg0);
}

codesign$1:::dynamic-create
/ arg1 != 0 && arg1 != kernel[pid].code /
{
	code[arg0].me = arg0;
	code[arg0].host = arg1;
	printf("%8u %s[%d]%s(%p,%p)\n",
		timestamp, execname, pid, probename, arg0, arg1);
}

security_debug$1:::sec-destroy
/ code[arg0].me == arg0 /
{
	code[arg0].me = 0;
	printf("%8u %s[%d]destroy code(%p)\n",
		timestamp, execname, pid, arg0);
}

security_debug$1:::sec-destroy
/ staticCode[arg0].me == arg0 /
{
	staticCode[arg0].me = 0;
	printf("%8u %s[%d]destroy staticCode(%p)\n",
		timestamp, execname, pid, arg0);
}


/*
 * Identification operations
 */
codesign$1:::guest-identify-*
{
	printf("%8u %s[%d]%s(%p,%d,%s[%p])\n",
		timestamp, execname, pid, probename,
		arg0, arg1, rep[staticCode[arg2].rep].path, arg2);
	code[arg0].staticCode = arg2;
}

codesign$1:::guest-cdhash-*
{
	self->hash = code[arg0].hash = (uint8_t *)copyin(arg1, sizeof(Hash));
	printf("%8u %s[%d]%s(%p,H\"%02x%02x%02x...%02x%02x\")\n",
		timestamp, execname, pid, probename, arg0,
		self->hash[0], self->hash[1], self->hash[2], self->hash[18], self->hash[19]);
}

codesign$1:::static-cdhash
{
	self->hash = staticCode[arg0].hash = (uint8_t *)copyin(arg1, sizeof(Hash));
	printf("%8u %s[%d]%s(%p,H\"%02x%02x%02x...%02x%02x\")\n",
		timestamp, execname, pid, probename, arg0,
		self->hash[0], self->hash[1], self->hash[2], self->hash[18], self->hash[19]);
}


/*
 * Guest registry/proxy management in securityd
 */
typedef struct {
	DTHandle guest;
	string path;
	uint32_t status;
	uint8_t *hash;
} SDGuest;
SDGuest guests[DTHandle, DTHandle];		/* host x guest */

securityd*:::host-register
{
	printf("%8u HOST DYNAMIC(%p,%d)\n",
		timestamp, arg0, arg1);
}

securityd*:::host-proxy
{
	printf("%8u HOST PROXY(%p,%d)\n",
		timestamp, arg0, arg1);
}

securityd*:::host-unregister
{
	printf("%8u HOST DESTROYED(%p)\n",
		timestamp, arg0);
}

securityd*:::guest-create
{
	guests[arg0, arg2].guest = arg2;
	guests[arg0, arg2].path = copyinstr(arg5);
	guests[arg0, arg2].status = arg3;
	printf("%8u GUEST CREATE(%p,%s[0x%x],host=0x%x,status=0x%x,flags=%d)\n",
		timestamp,
		arg0, guests[arg0, arg2].path, arg2, arg1, arg3, arg4);
}

securityd*:::guest-cdhash
/ arg2 != 0 /
{
	self->hash = guests[arg0, arg1].hash = (uint8_t *)copyin(arg2, sizeof(Hash));
	printf("%8u GUEST HASH(%p,%s[0x%x],H\"%02x%02x%02x...%02x%02x\")\n",
		timestamp,
		arg0, guests[arg0, arg1].path, arg1,
		self->hash[0], self->hash[1], self->hash[2], self->hash[18], self->hash[19]);
}

securityd*:::guest-cdhash
/ arg2 == 0 /
{
	printf("%8u GUEST HASH(%p,%s[0x%x],NONE)\n",
		timestamp, arg0, guests[arg0, arg1].path, arg1);
}

securityd*:::guest-change
{
	printf("%8u GUEST CHANGE(%p,%s[0x%x],status=0x%x)\n",
		timestamp,
		arg0, guests[arg0, arg1].path, arg1, arg2);
}

securityd*:::guest-destroy
{
	printf("%8u GUEST DESTROY(%p,%s[0x%x])\n",
		timestamp,
		arg0, guests[arg0, arg1].path, arg1);
}


/*
 * Signing Mach-O allocation tracking
 */
codesign$1:::allocate-arch
{
	printf("%8u %s[%d]%s(%s,%d)\n",
		timestamp, execname, pid, probename, copyinstr(arg0), arg1);
}

codesign$1:::allocate-archn
{
	printf("%8u %s[%d]%s((0x%x,0x%x),%d)\n",
		timestamp, execname, pid, probename, arg0, arg1,  arg2);
}

codesign$1:::allocate-write
{
	printf("%8u %s[%d]%s(%s,offset 0x%x,%d of %d)\n",
		timestamp, execname, pid, probename,
		copyinstr(arg0), arg1, arg2, arg3);
}

codesign$1:::allocate-validate
{
	printf("%8u %s[%d]%s(%s,%d)\n",
		timestamp, execname, pid, probename, copyinstr(arg0), arg1);
}


/*
 * Evaluation tracking
 */
codesign$1:::eval-dynamic-start
{
	printf("%8u %s[%d]%s(%p,%s)\n",
		timestamp, execname, pid, probename, arg0, copyinstr(arg1));
}

codesign$1:::eval-dynamic-end
{
	printf("%8u %s[%d]%s(%p)\n",
		timestamp, execname, pid, probename, arg0);
}

