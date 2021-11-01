/*
 * DTrace static providers at the Code Signing layer
 */
#define int32_t int
#define uint32_t unsigned
#define mach_port_t uint32_t


/*
 * Basic semantic events of the code signing subsystem
 */
provider codesign {
	probe diskrep__create__macho(void *me, const char *path, const void *ctx);
	probe diskrep__create__bundle__path(void *me, const char *path, void *ctx, void *exec);
	probe diskrep__create__bundle__ref(void *me, void *cfbundle, void *ctx, void *exec);
	probe diskrep__create__file(void *me, const char *path);
	probe diskrep__create__cfm(void *me, const char *path);
	probe diskrep__create__slc(void *me, const char *path);
	probe diskrep__create__detached(void *me, void *orig, const char *source, void *glob);
	probe diskrep__create__kernel(void *me);
	probe diskrep__destroy(void *me);

	probe static__create(void *me, void *host);
	probe dynamic__create(void *me, void *rep);
	
	probe static__cdhash(void *me, const void *cdhash, uint32_t length);
	probe static__attach__explicit(void *me, void *rep);
	probe static__attach__system(void *me, void *rep);

	probe eval__dynamic__start(void *me, const char *path);
	probe eval__dynamic__end(void *me);
	probe eval__dynamic__root(void *me);
	
	probe eval__static__start(void *me, const char *path);
	probe eval__static__end(void *me);
	probe eval__static__reset(void *me);
	
	probe eval__static__executable__start(void *me, const char *path, uint32_t pages);
	probe eval__static__executable__fail(void *me, uint32_t badPage);
	probe eval__static__executable__end(void *me);
	probe eval__static__resources__start(void *me, const char *path, int count);
	probe eval__static__resources__end(void *me);
	
	probe eval__static__directory(void *me);
	probe eval__static__intreq__start(void *me, uint32_t reqType, void *target, int32_t nullError);
	probe eval__static__intreq__end(void *me);
	
	probe eval__static__signature__start(void *me, const char *path);
	probe eval__static__signature__adhoc(void *me);
	probe eval__static__signature__result(void *me, uint32_t result, uint32_t chainLength);
	probe eval__static__signature__expired(void *me);
	probe eval__static__signature__end(void *me);

	probe eval__reqint__start(const void *reqdata, uint32_t reqlength);
	probe eval__reqint__end(const void *reqdata, uint32_t result);
	probe eval__reqint__op(uint32_t opcode, uint32_t offset);
	probe eval__reqint__unknown_false(uint32_t opcode);
	probe eval__reqint__unknown_skipped(uint32_t opcode);
	probe eval__reqint__fragment__load(const char *type, const char *name, const void *req);
	probe eval__reqint__fragment__hit(const char *type, const char *name);
	
	probe guest__hostingport(void *host, mach_port_t hostingPort);
	probe guest__locate__generic(void *host, uint32_t *guestPath, uint32_t guestPathLength, mach_port_t subport);
	probe guest__identify__process(void *guest, uint32_t guestPid, void *code);
	probe guest__cdhash__process(void *code, const void *cdhash, uint32_t length);
	probe guest__identify__generic(void *guest, uint32_t guestRef, void *code);
	probe guest__cdhash__generic(void *code, const void *cdhash, uint32_t length);
	
	probe allocate__validate(const char *path, uint32_t pid);
	probe allocate__arch(const char *arch, uint32_t size);
	probe allocate__archn(uint32_t cputype, uint32_t cpusubtype, uint32_t size);
	probe allocate__write(const char *arch, off_t offset, uint32_t length, uint32_t available);
	
	probe sign__dep__macho(void *me, const char *name, const void *requirement);
	probe sign__dep__interp(void *me, const char *name, const void *requirement);

	probe load__antlr();
};


provider syspolicy {
	probe assess_api(const char *path, int type, uint64_t flags);
	
	probe assess__outcome__accept(const char *path, int type, const char *label, const void *cdhash);
	probe assess__outcome__deny(const char *path, int type, const char *label, const void *cdhash);
	probe assess__outcome__default(const char *path, int type, const char *label, const void *cdhash);
	probe assess__outcome__unsigned(const char *path, int type);
	probe assess__outcome__broken(const char *path, int type, bool exception_made);

	probe recorder_mode(const char *path, int type, const char *label, const void *cdhash, int flags);
	probe recorder_mode_adhoc_path(const char *path, int type, const char *sig_path);	// path containing adhoc signature recorded
	
	probe assess_cache_hit();
	probe assess_local();
	probe assess_remote();
};
