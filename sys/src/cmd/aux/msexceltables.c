/* msexceltables.c    Steve Simon    5-Jan-2005 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

enum {
	Tillegal = 0,
	Tnumber,		// cell types
	Tlabel,
	Tindex,
	Tbool,
	Terror,

	Ver8 = 0x600,		// only BIFF8 and BIFF8x files support unicode
};
	
	
typedef struct Biff Biff;
typedef struct Col Col;
typedef struct Row Row;

struct Row {
	Row *next;
	int r;
	Col *col;
};

struct Col {
	Col *next;
	int c;
	int f;
	int type;
	union {
		int index;
		int error;
		int bool;
		char *label;
		double number;
	};
};

struct  Biff {
	Biobuf *bp;
	int op;
	int len;
};

// options
static int Nopad = 0;		// disable padding cells to colum width
static int Trunc = 0;		// truncate cells to colum width
static int All = 0;		// dump all sheet types, Worksheets only by default
static char *Delim = " ";	// field delimiter
static int Debug = 0;

// file scope
static int Defwidth = 10;	// default colum width if non given
static int Biffver;		// file vesion
static int Datemode;		// date ref: 1899-Dec-31 or 1904-jan-1
static char **Strtab = nil;	// label contents heap
static int Nstrtab = 0;		// # of above
static int *Xf;			// array of extended format indices
static int Nxf = 0;		// # of above
static Biobuf *bo;		// stdout (sic)

// table scope
static int *Width = nil;	// array of colum widths
static int Nwidths = 0;		// # of above
static int Ncols = -1;		// max colums in table used
static int Content = 0;		// type code for contents of sheet
static Row *Root = nil;		// one worksheet's worth of cells
				
static char *Months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static char *Errmsgs[] = {
	[0x0]	"#NULL!",	// intersection of two cell ranges is empty
	[0x7]	"#DIV/0!",	// division by zero	
	[0xf]	"#VALUE!",	// wrong type of operand
	[0x17]	"#REF!",	// illegal or deleted cell reference
	[0x1d]	"#NAME?",	// wrong function or range name
	[0x24]	"#NUM!",	// value range overflow
	[0x2a]	"#N/A!",	// argument of function not available
};

	
void
cell(int r, int c, int f, int type, void *val)
{
	Row *row, *nrow;
	Col *col, *ncol;

	if (c > Ncols)
		Ncols = c;

	if ((ncol = malloc(sizeof(Col))) == nil)
		sysfatal("no memory\n");
	ncol->c = c;
	ncol->f = f;
	ncol->type = type;
	ncol->next = nil;

	switch(type){
	case Tnumber:	ncol->number = *(double *)val;	break;
	case Tlabel:	ncol->label = (char *)val;	break;
	case Tindex:	ncol->index = *(int *)val;	break;
	case Tbool:	ncol->bool = *(int *)val;	break;
	case Terror:	ncol->error = *(int *)val;	break;
	default:	sysfatal("can't happen error\n");
	}

	if (Root == nil || Root->r > r){
		if ((nrow = malloc(sizeof(Row))) == nil)
			sysfatal("no memory\n");
		nrow->col = ncol;
		ncol->next = nil;
		nrow->r = r;
		nrow->next = Root;
		Root = nrow;
		return;
	}

	for (row = Root; row; row = row->next){
		if (row->r == r){
			if (row->col->c > c){
				ncol->next = row->col;
				row->col = ncol;
				return;
			}
			else{
				for (col = row->col; col; col = col->next)
					if (col->next == nil || col->next->c > c){
						ncol->next = col->next;
						col->next = ncol;
						return;
					}
			}
		}

		if (row->next == nil || row->next->r > r){
			if ((nrow = malloc(sizeof(Row))) == nil)
				sysfatal("no memory\n");
			nrow->col = ncol;
			nrow->r = r;
			nrow->next = row->next;
			row->next = nrow;
			return;
		}
	}
	sysfatal("cannot happen error\n");
}

void
numfmt(int fmt, int min, int max, double num)
{
	long t;
	char buf[1024];
	struct Tm *tm;

	/* Beware - These epochs are wrong, this
	 * is to remain compatible with Lotus-123
	 * which believed 1900 was a leap year
	 */
	if (Datemode)
		t = (num-24107)*60*60*24;	// epoch = 1/1/1904
	else
		t = (num-25569)*60*60*24;	// epoch = 31/12/1899
	tm = localtime(t);

	if (fmt == 9)
		snprint(buf, sizeof(buf),"%.0f%%", num);
	else
	if (fmt == 10)
		snprint(buf, sizeof(buf),"%f%%", num);
	else
	if (fmt == 11 || fmt == 48)
		snprint(buf, sizeof(buf),"%e", num);
	else
	if (fmt >= 14 && fmt <= 17)
		snprint(buf, sizeof(buf),"%d-%s-%d",
			tm->mday, Months[tm->mon], tm->year+1900);
	else
	if ((fmt >= 18 && fmt <= 21) || (fmt >= 45 && fmt <= 47))
		snprint(buf, sizeof(buf),"%02d:%02d:%02d", tm->hour, tm->min, tm->sec);
	else
	if (fmt == 22)
		snprint(buf, sizeof(buf),"%02d:%02d:%02d %d-%s-%d",
			tm->hour, tm->min, tm->sec,
			tm->mday, Months[tm->mon], tm->year+1900);
	else
		snprint(buf, sizeof(buf),"%g", num);
	Bprint(bo, "%-*.*q", min, max, buf);
}

void
dump(void)
{
	Row *r;
	Col *c;
	int i, min, max;

	for (r = Root; r; r = r->next){
		for (c = r->col; c; c = c->next){
			if (c->c < 0 || c->c >= Nwidths || (min = Width[c->c]) == 0)
				min = Defwidth;
			if ((c->next && c->c == c->next->c) || Nopad)
				min = 0;
			max = -1;
			if (Trunc && min > 2)
				max = min -2;		// FIXME: -2 because of bug %q format ?

			switch(c->type){
			case Tnumber:
				if (Xf[c->f] == 0)
					Bprint(bo, "%-*.*g", min, max, c->number);
				else
					numfmt(Xf[c->f], min, max, c->number);
				break;
			case Tlabel:
				Bprint(bo, "%-*.*q", min, max, c->label);
				break;
			case Tbool:
				Bprint(bo, "%-*.*s", min, max, (c->bool)? "True": "False");
				break;
			case Tindex:
				if (c->error < 0 || c->error >= Nstrtab)
					sysfatal("SST string out of range - corrupt file?\n");
				Bprint(bo, "%-*.*q", min, max, Strtab[c->index]);
				break;
			case Terror:
				if (c->error < 0 || c->error >= nelem(Errmsgs))
					Bprint(bo, "#ERR=%d", c->index);
				else
					Bprint(bo, "%-*.*q", min, max, Errmsgs[c->error]);
				break;
			default:
				sysfatal("cannot happen error\n");
				break;
			}

			if (c->next){
				if (c->next->c == c->c)		// bar charts
					Bprint(bo, "=");
				else{
					Bprint(bo, "%s", Delim);
					for (i = c->c; c->next && i < c->next->c -1; i++)
						Bprint(bo, "%-*.*s%s", min, max, "", Delim);
				}
			}
		}
		if (r->next)
			for (i = r->r; i < r->next->r; i++)
				Bprint(bo, "\n");

	}
	Bprint(bo, "\n");
}

void
release(void)
{
	Row *r, *or;
	Col *c, *oc;

	r = Root;
	while(r){
		c = r->col;
		while(c){
			if (c->type == Tlabel)
				free(c->label);
			oc = c;
			c = c->next;
			free(oc);
		}
		or = r;
		r = r->next;
		free(or);
	}
	Root = nil;

	free(Width);
	Width = nil;
	Nwidths = 0;
	Ncols = -1;
}

void
skip(Biff *b, int len)
{
	if (Bseek(b->bp, len, 1) == -1)
		sysfatal("seek failed - %r\n");
	b->len -= len;
}

void
gmem(Biff *b, void *p, int n)
{
	if (b->len < n)
		sysfatal("short record %d < %d\n", b->len, n);
	if (Bread(b->bp, p, n) != n)
		sysfatal("unexpected EOF - %r\n");
	b->len -= n;
}

void
xd(Biff *b)
{
	uvlong off;
	uchar buf[16];
	int addr, got, n, i, j;

	addr = 0;
	off = Boffset(b->bp);
	while (addr < b->len){
		n = (b->len >= sizeof(buf))? sizeof(buf): b->len;
		got = Bread(b->bp, buf, n);

		Bprint(bo, "	%6d  ", addr);
		addr += n;

		for (i = 0; i < got; i++)
			Bprint(bo, "%02x ", buf[i]);
		for (j = i; j < 16; j++)
			Bprint(bo, "   ");
		Bprint(bo, "  ");
		for (i = 0; i < got; i++)
			Bprint(bo, "%c", isprint(buf[i])? buf[i]: '.');
		Bprint(bo, "\n");
	}
	Bseek(b->bp, off, 0);
	off = Boffset(b->bp);
}

static int 
getrec(Biff *b)
{
	int c;
	if ((c = Bgetc(b->bp)) == -1)
		return -1;		// real EOF
	b->op = c;
	if ((c = Bgetc(b->bp)) == -1)
		sysfatal("unexpected EOF - %r\n");
	b->op |= c << 8;
	if ((c = Bgetc(b->bp)) == -1)
		sysfatal("unexpected EOF - %r\n");
	b->len = c;
	if ((c = Bgetc(b->bp)) == -1)
		sysfatal("unexpected EOF - %r\n");
	b->len |= c << 8;
	if (b->op == 0 && b->len == 0)
		return -1;
	if (Debug){
		Bprint(bo, "op=0x%x len=%d\n", b->op, b->len);
		xd(b);
	}
	return 0;
}

static uvlong
gint(Biff *b, int n)
{
	int i, c;
	uvlong vl, rc;

	if (b->len < n)
		return -1;
	rc = 0;
	for (i = 0; i < n; i++){
		if ((c = Bgetc(b->bp)) == -1)
			sysfatal("unexpected EOF - %r\n");
		b->len--;
		vl = c;
		rc |= vl << (8*i);
	}
	return rc;
}

double
grk(Biff *b)
{
	int f;
	uvlong n;
	double d;

	n = gint(b, 4);
	f = n & 3;
	n &= ~3LL;
	if (f & 2){
		d = n / 4.0;
	}
	else{
		n <<= 32;
		memcpy(&d, &n, sizeof(d));
	}

	if (f & 1)
		d /= 100.0;
	return d;
}

double
gdoub(Biff *b)
{
	double d;
	uvlong n = gint(b, 8);
	memcpy(&d, &n, sizeof(n));
	return d;
}

char *
gstr(Biff *b, int len_width)
{
	Rune r;
	int nch, sz, len, opt;
	char *buf, *p;

	if (b->len < len_width){
		if (getrec(b) == -1)
			sysfatal("expected CONTINUE, got EOF\n");
		if (b->op != 0x03c)	
			sysfatal("expected CONTINUE, got op=0x%x\n", b->op);
	}

	len = gint(b, len_width);
	if (Biffver != Ver8){
		if ((buf = calloc(len+1, sizeof(char))) == nil)
			sysfatal("no memory\n");
		gmem(b, buf, len);
		return buf;
	}


	if ((buf = calloc(len+1, sizeof(char)*UTFmax)) == nil)
		sysfatal("no memory\n");
	p = buf;

	if (len == 0)
		return buf;

	nch = 0;
	while (1){
		opt = gint(b, 1);
		sz = (opt & 1)? sizeof(Rune): sizeof(char);
		while(b->len > 0){
			r = gint(b, sz);
			p += runetochar(p, &r);
			if (++nch >= len){
				return buf;
			}
		}
		if (getrec(b) == -1)
			sysfatal("expected CONTINUE, got EOF\n");
		if (b->op != 0x03c)	
			sysfatal("expected CONTINUE, got op=0x%x\n", b->op);
	}
	sysfatal("cannot ever happen error\n");
	return buf;
}

void
sst(Biff *b)
{
	int n;
	
	skip(b, 4);			// total # strings
	Nstrtab = gint(b, 4);		// # unique strings
	if ((Strtab = calloc(Nstrtab, sizeof(char *))) == nil)
		sysfatal("no memory\n");
	for (n = 0; n < Nstrtab; n++)
		Strtab[n] = gstr(b, 2);
}

void
boolerr(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// col
	int f = gint(b, 2);		// formatting ref
	int v = gint(b, 1);		// bool value / err code
	int t = gint(b, 1);		// type
	cell(r, c, f, (t)? Terror: Tbool, &v);
}

void
rk(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// col
	int f = gint(b, 2);		// formatting ref
	double v = grk(b);		// value
	cell(r, c, f, Tnumber, &v);
}

void
mulrk(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// first col
	while (b->len >= 6){
		int f = gint(b, 2);	// formatting ref
		double v = grk(b);	// value
		cell(r, c++, f, Tnumber, &v);
	}
}

void
number(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// col
	int f = gint(b, 2);		// formatting ref
	double v = gdoub(b);		// double 
	cell(r, c, f, Tnumber, &v);
}

void
label(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// col
	int f = gint(b, 2);		// formatting ref
	char *s = gstr(b, 2);		// byte string
	cell(r, c, f, Tlabel, s);
}


void
labelsst(Biff *b)
{
	int r = gint(b, 2);		// row
	int c = gint(b, 2);		// col
	int f = gint(b, 2);		// formatting ref
	int i = gint(b, 2);		// sst string ref
	cell(r, c, f, Tindex, &i);
}

void
bof(Biff *b)
{
	Biffver = gint(b, 2);
	Content = gint(b, 2);	
}

void
defcolwidth(Biff *b)
{
	Defwidth = gint(b, 2);
}

void
datemode(Biff *b)
{
	Datemode = gint(b, 2);
}

void
eof(Biff *b)
{
	int i;
	struct {
		int n;
		char *s;
	} names[] = {
		0x005,	"Workbook globals",
		0x006,	"Visual Basic module",
		0x010,	"Worksheet",
		0x020,	"Chart",
		0x040,	"Macro sheet",
		0x100,	"Workspace file",
	};
		
	if (Ncols != -1){
		if (All){
			for (i = 0; i < nelem(names); i++)
				if (names[i].n == Content){
					Bprint(bo, "\n# contents %s\n", names[i].s);
					dump();
				}
		}
		else 
		if (Content == 0x10)
			dump();
	}
	release();
	USED(b);
}

void
colinfo(Biff *b)
{
	int c;
	int c1 = gint(b, 2);
	int c2 = gint(b, 2);
	int w  = gint(b, 2);

	if (c2 >= Nwidths){
		Nwidths = c2+20;
		if ((Width = realloc(Width, Nwidths*sizeof(int))) == nil)
			sysfatal("no memory\n");
	}

	w /= 256;

	if (w > 100)
		w = 100;
	if (w < 0)
		w = 0;

	for (c = c1; c <= c2; c++)
		Width[c] = w;
}

void
xf(Biff *b)
{
	int fmt;
	static int nalloc = 0;

	skip(b, 2);
	fmt = gint(b, 2);
	if (nalloc >= Nxf){
		nalloc += 20;
		if ((Xf = realloc(Xf, nalloc*sizeof(int))) == nil)
			sysfatal("no memory\n");
	}
	Xf[Nxf++] = fmt;
}

void
writeaccess(Biff *b)
{
	Bprint(bo, "# author %s\n", gstr(b, 2));
}

void
codepage(Biff *b)
{
	int codepage = gint(b, 2);
	if (codepage != 1200)				// 1200 == UTF-16
		Bprint(bo, "# codepage %d\n", codepage);
}

void
xls2csv(Biobuf *bp)
{
	int i;
	Biff biff, *b;
	struct {
		int op;
		void (*func)(Biff *);
	} dispatch[] = {
		0x00a,	eof,
		0x022,	datemode,
		0x042,	codepage,
		0x055,	defcolwidth,
		0x05c,	writeaccess,
		0x07d,	colinfo,
		0x0bd,	mulrk,
		0x0fc,	sst,
		0x0fd,	labelsst,
		0x203,	number,
		0x204,	label,
		0x205,	boolerr,
		0x27e,	rk,
		0x809,	bof,
		0x0e0,	xf,
	};		
	
	b = &biff;
	b->bp = bp;
	while(getrec(b) != -1){
		for (i = 0; i < nelem(dispatch); i++)
			if (b->op == dispatch[i].op)
				(*dispatch[i].func)(b);
		skip(b, b->len);
	}
}

void
usage(void)
{
	fprint(2, "usage: %s [-nta] [-d delim] file.xls\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int i;
	Biobuf bin, bout, *bp;
	
	ARGBEGIN{
	case 'n':
		Nopad = 1;
		break;
	case 't':
		Trunc = 1;
		break;
	case 'a':
		All = 1;
		break;
	case 'd':
		Delim = EARGF(usage());
		break;
	case 'D':
		Debug = 1;
		break;
	default:
		usage();
		break;
	}ARGEND;

	if (argc != 1)
		usage();

	bo = &bout;
	quotefmtinstall();
	Binit(bo, OWRITE, 1);

	if(argc > 0) {
		for(i = 0; i < argc; i++){
			if ((bp = Bopen(argv[i], OREAD)) == nil)
				sysfatal("%s cannot open - %r\n", argv[i]);
			xls2csv(bp);
			Bterm(bp);
		}
	} else {
		Binit(&bin, 0, OREAD);
		xls2csv(&bin);
	}
	exits(0);
}
