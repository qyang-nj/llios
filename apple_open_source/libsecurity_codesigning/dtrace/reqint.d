#!/usr/sbin/dtrace -q -s


string opnames[unsigned];	/* common opcode names */


dtrace:::BEGIN
{
	printf("ready...\n");
	opnames[0] = "never";
	opnames[1] = "always";
	opnames[2] = "identifier...";
	opnames[3] = "anchor apple";
	opnames[4] = "anchor = ...";
	opnames[5] = "!legacy infokey!";
	opnames[6] = "AND";
	opnames[7] = "OR";
	opnames[8] = "cdhash";
	opnames[9] = "NOT";
	opnames[10] = "info[...]";
	opnames[11] = "cert[subject...]";
	opnames[12] = "anchor trusted...";
	opnames[13] = "anchor trusted...";
	opnames[14] = "cert[field...]";
	opnames[15] = "anchor apple generic";
	opnames[16] = "entitlement[...]";
	opnames[17] = "cert[policy...]";
	opnames[18] = "anchor NAMED";
	opnames[19] = "(NAMED)";
}


codesign*:::eval-reqint-start
{
	printf("%8u %s[%d] START(%p,%d)\n",
		timestamp, execname, pid,
		arg0, arg1);
}

codesign*:::eval-reqint-end
{
	@eval[arg1] = count();
}

codesign*:::eval-reqint-end
/ arg1 == 0 /
{
	printf("%8u %s[%d] SUCCESS\n",
		timestamp, execname, pid);
}

codesign*:::eval-reqint-end
/ arg1 == 4294900246 /
{
	printf("%8u %s[%d] FAIL\n",
		timestamp, execname, pid);
}

codesign*:::eval-reqint-end
/ arg1 != 4294900246 && arg1 != 0 /
{
	printf("%8u %s[%d] FAIL(%d)\n",
		timestamp, execname, pid,
		arg1);
}

codesign*:::eval-reqint-unknown*
{
	printf("%8u %s[%d] %s(%d)\n",
		timestamp, execname, pid, probename,
		arg0);
}

codesign*:::eval-reqint-fragment-load
/ arg2 != 0 /
{
	printf("%8u %s[%d] frag-load(%s,%s,%p)\n",
		timestamp, execname, pid,
		copyinstr(arg0), copyinstr(arg1), arg2);
	@fragload[copyinstr(arg0), copyinstr(arg1)] = count();
	@fraguse[copyinstr(arg0), copyinstr(arg1)] = count();
}

codesign*:::eval-reqint-fragment-load
/ arg2 == 0 /
{
	printf("%8u %s[%d] frag-load(%s,%s,FAILED)\n",
		timestamp, execname, pid,
		copyinstr(arg0), copyinstr(arg1));
	@fragload[copyinstr(arg0), copyinstr(arg1)] = count();
	@fraguse[copyinstr(arg0), copyinstr(arg1)] = count();
}

codesign*:::eval-reqint-fragment-hit
{
	printf("%8u %s[%d] frag-hit(%s,%s)\n",
		timestamp, execname, pid,
		copyinstr(arg0), copyinstr(arg1));
	@fraguse[copyinstr(arg0), copyinstr(arg1)] = count();
}


/*
 * Trace opcodes as they're encountered and evaluated
 */
codesign*:::eval-reqint-op
{
	self->traced = 0;
	@opcodes[arg0] = count();
}

codesign*:::eval-reqint-op
/ !self->traced /
{
	printf("%8u %s[%d] %s\n", timestamp, execname, pid,
		opnames[arg0]);
}


/*
 * Print out aggregates at the end
 */
dtrace:::END
{
	printf("\nREQUIREMENT EVALUATIONS:\n");
	printa("\t%d (%@d)\n", @eval);

	printf("\nREQUIREMENT OPCODES EVALUATED:\n");
	printa("\t%5d (%@d)\n", @opcodes);
	
	printf("\nFRAGMENTS LOADED:\n");
	printa("\t%s %s (%@d)\n", @fragload);
}
