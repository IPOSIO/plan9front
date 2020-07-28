#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

void (*_forker)(void(*)(void*), void*, int);

static char Ebadattach[] = "unknown specifier in attach";
static char Ebadoffset[] = "bad offset";
static char Ebadcount[] = "bad count";
static char Ebotch[] = "9P protocol botch";
static char Ecreatenondir[] = "create in non-directory";
static char Edupfid[] = "duplicate fid";
static char Eduptag[] = "duplicate tag";
static char Eisdir[] = "is a directory";
static char Enocreate[] = "create prohibited";
static char Enomem[] = "out of memory";
static char Enoremove[] = "remove prohibited";
static char Enostat[] = "stat prohibited";
static char Enotfound[] = "file not found";
static char Enowrite[] = "write prohibited";
static char Enowstat[] = "wstat prohibited";
static char Eperm[] = "permission denied";
static char Eunknownfid[] = "unknown fid";
static char Ebaddir[] = "bad directory in wstat";
static char Ewalknodir[] = "walk in non-directory";

static void
setfcallerror(Fcall *f, char *err)
{
	f->ename = err;
	f->type = Rerror;
}

static void
changemsize(Srv *srv, int msize)
{
	if(srv->rbuf && srv->wbuf && srv->msize == msize)
		return;
	qlock(&srv->rlock);
	qlock(&srv->wlock);
	srv->msize = msize;
	free(srv->rbuf);
	free(srv->wbuf);
	srv->rbuf = emalloc9p(msize);
	srv->wbuf = emalloc9p(msize);
	qunlock(&srv->rlock);
	qunlock(&srv->wlock);
}

static Req*
getreq(Srv *s)
{
	long n;
	uchar *buf;
	Fcall f;
	Req *r;

	qlock(&s->rlock);
	n = read9pmsg(s->infd, s->rbuf, s->msize);
	if(n <= 0){
		qunlock(&s->rlock);
		return nil;
	}

	buf = emalloc9p(n);
	memmove(buf, s->rbuf, n);
	qunlock(&s->rlock);

	if(convM2S(buf, n, &f) != n){
		free(buf);
		return nil;
	}

	if((r=allocreq(s->rpool, f.tag)) == nil){	/* duplicate tag: cons up a fake Req */
		r = emalloc9p(sizeof *r);
		incref(&r->ref);
		r->tag = f.tag;
		r->ifcall = f;
		r->error = Eduptag;
		r->buf = buf;
		r->responded = 0;
		r->type = 0;
		r->srv = s;
		r->pool = nil;
if(chatty9p)
	fprint(2, "<-%d- %F: dup tag\n", s->infd, &f);
		return r;
	}

	r->srv = s;
	r->responded = 0;
	r->buf = buf;
	r->ifcall = f;
	memset(&r->ofcall, 0, sizeof r->ofcall);
	r->type = r->ifcall.type;

if(chatty9p)
	if(r->error)
		fprint(2, "<-%d- %F: %s\n", s->infd, &r->ifcall, r->error);
	else	
		fprint(2, "<-%d- %F\n", s->infd, &r->ifcall);

	return r;
}

static void
filewalk(Req *r)
{
	int i;
	File *f;

	f = r->fid->file;
	assert(f != nil);

	incref(f);
	for(i=0; i<r->ifcall.nwname; i++)
		if(f = walkfile(f, r->ifcall.wname[i]))
			r->ofcall.wqid[i] = f->qid;
		else
			break;

	r->ofcall.nwqid = i;
	if(f){
		r->newfid->file = f;
		r->newfid->qid = r->newfid->file->qid;
	}
	respond(r, nil);
}

void
walkandclone(Req *r, char *(*walk1)(Fid*, char*, void*), char *(*clone)(Fid*, Fid*, void*), void *arg)
{
	int i;
	char *e;

	if(r->fid == r->newfid && r->ifcall.nwname > 1){
		respond(r, "lib9p: unused documented feature not implemented");
		return;
	}

	if(r->fid != r->newfid){
		r->newfid->qid = r->fid->qid;
		if(clone && (e = clone(r->fid, r->newfid, arg))){
			respond(r, e);
			return;
		}
	}

	e = nil;
	for(i=0; i<r->ifcall.nwname; i++){
		if(e = walk1(r->newfid, r->ifcall.wname[i], arg))
			break;
		r->ofcall.wqid[i] = r->newfid->qid;
	}

	r->ofcall.nwqid = i;
	if(e && i==0)
		respond(r, e);
	else
		respond(r, nil);
}

static void
sversion(Srv *srv, Req *r)
{
	if(srv->rref.ref != 1){
		respond(r, Ebotch);
		return;
	}
	if(strncmp(r->ifcall.version, "9P", 2) != 0){
		r->ofcall.version = "unknown";
		r->ofcall.msize = 256;
		respond(r, nil);
		return;
	}
	r->ofcall.version = "9P2000";
	if(r->ifcall.msize < 256){
		respond(r, "version: message size too small");
		return;
	}
	if(r->ifcall.msize < 1024*1024)
		r->ofcall.msize = r->ifcall.msize;
	else
		r->ofcall.msize = 1024*1024;
	respond(r, nil);
}

static void
rversion(Req *r, char *error)
{
	if(error == nil)
		changemsize(r->srv, r->ofcall.msize);
}

static void
sauth(Srv *srv, Req *r)
{
	char e[ERRMAX];

	if((r->afid = allocfid(srv->fpool, r->ifcall.afid)) == nil){
		respond(r, Edupfid);
		return;
	}
	if(srv->auth)
		srv->auth(r);
	else{
		snprint(e, sizeof e, "%s: authentication not required", argv0);
		respond(r, e);
	}
}
static void
rauth(Req *r, char *error)
{
	if(r->afid == nil)
		return;
	if(error){
		closefid(removefid(r->srv->fpool, r->afid->fid));
		return;
	}
	if(r->afid->omode == -1)
		r->afid->omode = ORDWR;
}

static void
sattach(Srv *srv, Req *r)
{
	if((r->fid = allocfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Edupfid);
		return;
	}
	r->afid = nil;
	if(r->ifcall.afid != NOFID && (r->afid = lookupfid(srv->fpool, r->ifcall.afid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	r->fid->uid = estrdup9p(r->ifcall.uname);
	if(srv->tree){
		r->fid->file = srv->tree->root;
		incref(r->fid->file);
		r->ofcall.qid = r->fid->file->qid;
		r->fid->qid = r->ofcall.qid;
	}
	if(srv->attach)
		srv->attach(r);
	else
		respond(r, nil);
	return;
}
static void
rattach(Req *r, char *error)
{
	if(error && r->fid)
		closefid(removefid(r->srv->fpool, r->fid->fid));
}

static void
sflush(Srv *srv, Req *r)
{
	r->oldreq = lookupreq(srv->rpool, r->ifcall.oldtag);
	if(r->oldreq == nil || r->oldreq == r)
		respond(r, nil);
	else if(srv->flush)
		srv->flush(r);
	else
		respond(r, nil);
}
static int
rflush(Req *r, char *error)
{
	Req *or;

	assert(error == nil);
	or = r->oldreq;
	if(or){
		qlock(&or->lk);
		if(or->responded == 0){
			or->flush = erealloc9p(or->flush, (or->nflush+1)*sizeof(or->flush[0]));
			or->flush[or->nflush++] = r;
			qunlock(&or->lk);
			return -1;		/* delay response until or is responded */
		}
		qunlock(&or->lk);
		closereq(or);
	}
	r->oldreq = nil;
	return 0;
}

static char*
oldwalk1(Fid *fid, char *name, void *arg)
{
	char *e;
	Qid qid;
	Srv *srv;

	srv = arg;
	e = srv->walk1(fid, name, &qid);
	if(e)
		return e;
	fid->qid = qid;
	return nil;
}

static char*
oldclone(Fid *fid, Fid *newfid, void *arg)
{
	Srv *srv;

	srv = arg;
	if(srv->clone == nil)
		return nil;
	return srv->clone(fid, newfid);
}

static void
swalk(Srv *srv, Req *r)
{
	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	if(r->fid->omode != -1){
		respond(r, "cannot clone open fid");
		return;
	}
	if(r->ifcall.nwname && !(r->fid->qid.type&QTDIR)){
		respond(r, Ewalknodir);
		return;
	}
	if(r->ifcall.fid != r->ifcall.newfid){
		if((r->newfid = allocfid(srv->fpool, r->ifcall.newfid)) == nil){
			respond(r, Edupfid);
			return;
		}
		r->newfid->uid = estrdup9p(r->fid->uid);
	}else{
		incref(&r->fid->ref);
		r->newfid = r->fid;
	}
	if(r->fid->file){
		filewalk(r);
	}else if(srv->walk1)
		walkandclone(r, oldwalk1, oldclone, srv);
	else if(srv->walk)
		srv->walk(r);
	else
		sysfatal("no walk function, no file trees");
}
static void
rwalk(Req *r, char *error)
{
	if(error || r->ofcall.nwqid < r->ifcall.nwname){
		if(r->ifcall.fid != r->ifcall.newfid && r->newfid)
			closefid(removefid(r->srv->fpool, r->newfid->fid));
		if (r->ofcall.nwqid==0){
			if(error==nil && r->ifcall.nwname!=0)
				r->error = Enotfound;
		}else
			r->error = nil;	// No error on partial walks
	}else{
		if(r->ofcall.nwqid == 0){
			/* Just a clone */
			r->newfid->qid = r->fid->qid;
		}else{
			/* if file trees are in use, filewalk took care of the rest */
			r->newfid->qid = r->ofcall.wqid[r->ofcall.nwqid-1];
		}
	}
}

static int
dirwritable(Fid *fid)
{
	File *f;

	f = fid->file;
	if(f){
		rlock(f);
		if(f->parent && !hasperm(f->parent, fid->uid, AWRITE)){
			runlock(f);
			return 0;
		}
		runlock(f);
	}
	return 1;
}

static void
sopen(Srv *srv, Req *r)
{
	int p;

	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	if(r->fid->omode != -1){
		respond(r, Ebotch);
		return;
	}
	if((r->fid->qid.type&QTDIR) && (r->ifcall.mode&~ORCLOSE) != OREAD){
		respond(r, Eisdir);
		return;
	}
	r->ofcall.qid = r->fid->qid;
	switch(r->ifcall.mode&3){
	default:
		respond(r, Ebotch);
		return;
	case OREAD:
		p = AREAD;	
		break;
	case OWRITE:
		p = AWRITE;
		break;
	case ORDWR:
		p = AREAD|AWRITE;
		break;
	case OEXEC:
		p = AEXEC;	
		break;
	}
	if(r->ifcall.mode&OTRUNC)
		p |= AWRITE;
	if((r->fid->qid.type&QTDIR) && p!=AREAD){
		respond(r, Eperm);
		return;
	}
	if(r->fid->file){
		if(!hasperm(r->fid->file, r->fid->uid, p)){
			respond(r, Eperm);
			return;
		}
		if((r->ifcall.mode&ORCLOSE) && !dirwritable(r->fid)){
			respond(r, Eperm);
			return;
		}
		r->ofcall.qid = r->fid->file->qid;
		if((r->ofcall.qid.type&QTDIR)
		&& (r->fid->rdir = opendirfile(r->fid->file)) == nil){
			respond(r, "opendirfile failed");
			return;
		}
	}
	if(srv->open)
		srv->open(r);
	else
		respond(r, nil);
}

static void
screate(Srv *srv, Req *r)
{
	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil)
		respond(r, Eunknownfid);
	else if(r->fid->omode != -1)
		respond(r, Ebotch);
	else if(!(r->fid->qid.type&QTDIR))
		respond(r, Ecreatenondir);
	else if(r->fid->file && !hasperm(r->fid->file, r->fid->uid, AWRITE))
		respond(r, Eperm);
	else if(srv->create)
		srv->create(r);
	else
		respond(r, Enocreate);
}

static void
ropen(Req *r, char *error)
{
	if(error)
		return;
	if(chatty9p)
		fprint(2, "fid mode is %x\n", (int)r->ifcall.mode);
	if(r->ofcall.qid.type&QTDIR)
		r->fid->diroffset = 0;
	r->fid->qid = r->ofcall.qid;
	r->fid->omode = r->ifcall.mode;
}

static void
sread(Srv *srv, Req *r)
{
	int o;

	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	o = r->fid->omode;
	if(o == -1){
		respond(r, Ebotch);
		return;
	}
	switch(o & 3){
	default:
		respond(r, Ebotch);
		return;
	case OREAD:
	case ORDWR:
	case OEXEC:
		break;
	}
	if((int)r->ifcall.count < 0){
		respond(r, Ebotch);
		return;
	}
	if(r->ifcall.offset < 0
	|| ((r->fid->qid.type&QTDIR) && r->ifcall.offset != 0 && r->ifcall.offset != r->fid->diroffset)){
		respond(r, Ebadoffset);
		return;
	}
	if(r->ifcall.count > srv->msize - IOHDRSZ)
		r->ifcall.count = srv->msize - IOHDRSZ;
	r->rbuf = emalloc9p(r->ifcall.count);
	r->ofcall.data = r->rbuf;
	if((r->fid->qid.type&QTDIR) && r->fid->file){
		r->ofcall.count = readdirfile(r->fid->rdir, r->rbuf, r->ifcall.count, r->ifcall.offset);
		respond(r, nil);
		return;
	}
	if(srv->read)
		srv->read(r);
	else
		respond(r, "no srv->read");
}
static void
rread(Req *r, char *error)
{
	if(error==nil && (r->fid->qid.type&QTDIR))
		r->fid->diroffset = r->ifcall.offset + r->ofcall.count;
}

static void
swrite(Srv *srv, Req *r)
{
	int o;

	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	o = r->fid->omode;
	if(o == -1){
		respond(r, Ebotch);
		return;
	}
	switch(o & 3){
	default:
		respond(r, Ebotch);
		return;
	case OWRITE:
	case ORDWR:
		break;
	}
	if(r->fid->qid.type&QTDIR){
		respond(r, Ebotch);
		return;
	}
	if((int)r->ifcall.count < 0){
		respond(r, Ebotch);
		return;
	}
	if(r->ifcall.offset < 0){
		respond(r, Ebotch);
		return;
	}
	if(r->ifcall.count > srv->msize - IOHDRSZ)
		r->ifcall.count = srv->msize - IOHDRSZ;
	if(srv->write)
		srv->write(r);
	else
		respond(r, Enowrite);
}
static void
rwrite(Req *r, char *error)
{
	if(error)
		return;
	if(r->fid->file)
		r->fid->file->qid.vers++;
}

static void
sclunk(Srv *srv, Req *r)
{
	if((r->fid = removefid(srv->fpool, r->ifcall.fid)) == nil)
		respond(r, Eunknownfid);
	else
		respond(r, nil);
}
static void
rclunk(Req*, char*)
{
}

static void
sremove(Srv *srv, Req *r)
{
	if((r->fid = removefid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	if(!dirwritable(r->fid)){
		respond(r, Eperm);
		return;
	}
	if(srv->remove)
		srv->remove(r);
	else
		respond(r, r->fid->file ? nil : Enoremove);
}
static void
rremove(Req *r, char *error, char *errbuf)
{
	if(error)
		return;
	if(r->fid->file){
		if(removefile(r->fid->file) < 0){
			snprint(errbuf, ERRMAX, "remove %s: %r", 
				r->fid->file->name);
			r->error = errbuf;
		}
		r->fid->file = nil;
	}
}

static void
sstat(Srv *srv, Req *r)
{
	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	if(r->fid->file){
		/* should we rlock the file? */
		r->d = r->fid->file->Dir;
		if(r->d.name)
			r->d.name = estrdup9p(r->d.name);
		if(r->d.uid)
			r->d.uid = estrdup9p(r->d.uid);
		if(r->d.gid)
			r->d.gid = estrdup9p(r->d.gid);
		if(r->d.muid)
			r->d.muid = estrdup9p(r->d.muid);
	}
	if(srv->stat)	
		srv->stat(r);	
	else if(r->fid->file)
		respond(r, nil);
	else
		respond(r, Enostat);
}
static void
rstat(Req *r, char *error)
{
	int n;
	uchar *statbuf;
	uchar tmp[BIT16SZ];

	if(error)
		return;
	if(convD2M(&r->d, tmp, BIT16SZ) != BIT16SZ){
		r->error = "convD2M(_,_,BIT16SZ) did not return BIT16SZ";
		return;
	}
	n = GBIT16(tmp)+BIT16SZ;
	statbuf = emalloc9p(n);
	if(statbuf == nil){
		r->error = "out of memory";
		return;
	}
	r->ofcall.nstat = convD2M(&r->d, statbuf, n);
	r->ofcall.stat = statbuf;	/* freed in closereq */
	if(r->ofcall.nstat <= BIT16SZ){
		r->error = "convD2M fails";
		free(statbuf);
		return;
	}
}

static void
swstat(Srv *srv, Req *r)
{
	if((r->fid = lookupfid(srv->fpool, r->ifcall.fid)) == nil){
		respond(r, Eunknownfid);
		return;
	}
	if(srv->wstat == nil){
		respond(r, Enowstat);
		return;
	}
	if(convM2D(r->ifcall.stat, r->ifcall.nstat, &r->d, (char*)r->ifcall.stat) != r->ifcall.nstat){
		respond(r, Ebaddir);
		return;
	}
	if(r->d.qid.path != ~0 && r->d.qid.path != r->fid->qid.path){
		respond(r, "wstat -- attempt to change qid.path");
		return;
	}
	if(r->d.qid.vers != ~0 && r->d.qid.vers != r->fid->qid.vers){
		respond(r, "wstat -- attempt to change qid.vers");
		return;
	}
	if(r->d.mode != ~0){
		if(r->d.mode & ~(DMDIR|DMAPPEND|DMEXCL|DMTMP|0777)){
			respond(r, "wstat -- unknown bits in mode");
			return;
		}
		if(r->d.qid.type != (uchar)~0 && r->d.qid.type != ((r->d.mode>>24)&0xFF)){
			respond(r, "wstat -- qid.type/mode mismatch");
			return;
		}
		if(((r->d.mode>>24) ^ r->fid->qid.type) & ~(QTAPPEND|QTEXCL|QTTMP)){
			respond(r, "wstat -- attempt to change qid.type");
			return;
		}
	} else {
		if(r->d.qid.type != (uchar)~0 && r->d.qid.type != r->fid->qid.type){
			respond(r, "wstat -- attempt to change qid.type");
			return;
		}
	}
	srv->wstat(r);
}
static void
rwstat(Req*, char*)
{
}

static void srvclose(Srv *);

static void
srvwork(void *v)
{
	Srv *srv = v;
	Req *r;

	while(r = getreq(srv)){
		incref(&srv->rref);
		if(r->error){
			respond(r, r->error);
			continue;	
		}
		qlock(&srv->slock);
		switch(r->ifcall.type){
		default:
			respond(r, "unknown message");
			break;
		case Tversion:	sversion(srv, r);	break;
		case Tauth:	sauth(srv, r);	break;
		case Tattach:	sattach(srv, r);	break;
		case Tflush:	sflush(srv, r);	break;
		case Twalk:	swalk(srv, r);	break;
		case Topen:	sopen(srv, r);	break;
		case Tcreate:	screate(srv, r);	break;
		case Tread:	sread(srv, r);	break;
		case Twrite:	swrite(srv, r);	break;
		case Tclunk:	sclunk(srv, r);	break;
		case Tremove:	sremove(srv, r);	break;
		case Tstat:	sstat(srv, r);	break;
		case Twstat:	swstat(srv, r);	break;
		}
		if(srv->sref.ref > 8 && srv->spid != getpid()){
			decref(&srv->sref);
			qunlock(&srv->slock);
			return;
		}
		qunlock(&srv->slock);
	}

	if(srv->end && srv->sref.ref == 1)
		srv->end(srv);
	if(decref(&srv->sref) == 0)
		srvclose(srv);
}

static void
srvclose(Srv *srv)
{
	if(srv->rref.ref || srv->sref.ref)
		return;

	if(chatty9p)
		fprint(2, "srvclose\n");

	free(srv->rbuf);
	srv->rbuf = nil;
	free(srv->wbuf);
	srv->wbuf = nil;
	srv->msize = 0;
	freefidpool(srv->fpool);
	srv->fpool = nil;
	freereqpool(srv->rpool);
	srv->rpool = nil;

	if(srv->free)
		srv->free(srv);
}

void
srvacquire(Srv *srv)
{
	incref(&srv->sref);
	qlock(&srv->slock);
}

void
srvrelease(Srv *srv)
{
	if(decref(&srv->sref) == 0){
		incref(&srv->sref);
		_forker(srvwork, srv, 0);
	}
	qunlock(&srv->slock);
}

void
srv(Srv *srv)
{
	fmtinstall('D', dirfmt);
	fmtinstall('F', fcallfmt);

	srv->spid = getpid();
	memset(&srv->sref, 0, sizeof(srv->sref));
	memset(&srv->rref, 0, sizeof(srv->rref));

	if(srv->fpool == nil)
		srv->fpool = allocfidpool(srv->destroyfid);
	if(srv->rpool == nil)
		srv->rpool = allocreqpool(srv->destroyreq);
	if(srv->msize == 0)
		srv->msize = 8192+IOHDRSZ;

	changemsize(srv, srv->msize);

	srv->fpool->srv = srv;
	srv->rpool->srv = srv;

	if(srv->start)
		srv->start(srv);

	incref(&srv->sref);
	srvwork(srv);
}

void
respond(Req *r, char *error)
{
	int i, m, n;
	char errbuf[ERRMAX];
	Srv *srv;

	srv = r->srv;
	assert(srv != nil);

	assert(r->responded == 0);
	r->error = error;

	switch(r->ifcall.type){
	/*
	 * Flush is special.  If the handler says so, we return
	 * without further processing.  Respond will be called
	 * again once it is safe.
	 */
	case Tflush:
		if(rflush(r, error)<0)
			return;
		break;
	case Tversion:	rversion(r, error);	break;
	case Tauth:	rauth(r, error);	break;
	case Tattach:	rattach(r, error);	break;
	case Twalk:	rwalk(r, error);	break;
	case Topen:	ropen(r, error);	break;
	case Tcreate:	ropen(r, error);	break;
	case Tread:	rread(r, error);	break;
	case Twrite:	rwrite(r, error);	break;
	case Tclunk:	rclunk(r, error);	break;
	case Tremove:	rremove(r, error, errbuf);	break;
	case Tstat:	rstat(r, error);	break;
	case Twstat:	rwstat(r, error);	break;
	}

	r->ofcall.tag = r->ifcall.tag;
	r->ofcall.type = r->ifcall.type+1;
	if(r->error)
		setfcallerror(&r->ofcall, r->error);

if(chatty9p)
	fprint(2, "-%d-> %F\n", srv->outfd, &r->ofcall);

	qlock(&srv->wlock);
	n = convS2M(&r->ofcall, srv->wbuf, srv->msize);
	if(n <= 0){
		fprint(2, "msize = %d n = %d %F\n", srv->msize, n, &r->ofcall);
		abort();
	}
	assert(n > 2);
	if(r->pool)	/* not a fake */
		closereq(removereq(r->pool, r->ifcall.tag));
	m = write(srv->outfd, srv->wbuf, n);
	if(m != n)
		fprint(2, "lib9p srv: write %d returned %d on fd %d: %r", n, m, srv->outfd);
	qunlock(&srv->wlock);

	qlock(&r->lk);	/* no one will add flushes now */
	r->responded = 1;
	qunlock(&r->lk);

	for(i=0; i<r->nflush; i++)
		respond(r->flush[i], nil);
	free(r->flush);
	r->flush = nil;
	r->nflush = 0;

	if(r->pool)
		closereq(r);
	else
		free(r);

	if(decref(&srv->rref) == 0)
		srvclose(srv);
}

void
responderror(Req *r)
{
	char errbuf[ERRMAX];
	
	rerrstr(errbuf, sizeof errbuf);
	respond(r, errbuf);
}
