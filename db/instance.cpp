// instance.cpp : Global state variables and functions.
//

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "db.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "json.h"
#include "reccache.h"

namespace mongo {

    int nloggedsome = 0;
#define LOGSOME if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    bool quota = false;
    bool slave = false;
    bool master = false; // true means keep an op log
    extern int curOp;
    bool autoresync = false;
    
    boost::mutex &dbMutex( *(new boost::mutex) );
    MutexInfo dbMutexInfo;
//int dbLocked = 0;

    string dbExecCommand;

    int port = DBPort;
    /* 0 = off; 1 = writes, 2 = reads, 3 = both
       7 = log a few reads, and all writes.
    */
    int opLogging = 0;

    int getOpLogging() {
        return opLogging;
    }
    OpLog _oplog;
//#define oplog (*(_oplog.f))
    long long oplogSize = 0;

    bool useCursors = true;
    bool useHints = true;
    
    void closeAllSockets();
    void flushOpLog( stringstream &ss ) {
        if( _oplog.f && _oplog.f->is_open() ) {
            ss << "flushing op log and files\n";
            _oplog.flush();
        }
    }

    int ctr = 0;
    bool quiet = false;
    bool cpu = false; // --cpu show cpu time periodically

    /* 0 = ok
       1 = kill current operation and reset this to 0
       future: maybe use this as a "going away" thing on process termination with a higher flag value 
    */
    int killCurrentOp = 0;

    CurOp currentOp;

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObj obj = currentOp.info();
        replyToQuery(0, m, dbresponse, obj);
    }

    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        AuthenticationInfo *ai = authInfo.get();
        if( ai == 0 || !ai->isAuthorized("admin") ) { 
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if( !dbMutexInfo.isLocked() ) 
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
        else {
            killCurrentOp = 1;
            obj = fromjson("{\"info\":\"attempting to kill op\"}");
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    // Returns false when request includes 'end'
    bool assembleResponse( Message &m, DbResponse &dbresponse ) {
        // before we lock...
        if ( m.data->operation() == dbQuery ) {
            const char *ns = m.data->_data + 4;
            if( strstr(ns, "$cmd.sys.") ) { 
                if( strstr(ns, "$cmd.sys.inprog") ) {
                    inProgCmd(m, dbresponse);
                    return true;
                }
                if( strstr(ns, "$cmd.sys.killop") ) { 
                    killOp(m, dbresponse);
                    return true;
                }
            }
        }

        dblock lk;

        stringstream ss;
        char buf[64];
        time_t now = time(0);
        currentOp.reset(now);

        time_t_to_String(now, buf);
        buf[20] = 0; // don't want the year
        ss << buf;

        Timer t;
        database = 0;

        int ms;
        bool log = false;
        currentOp.op = curOp = m.data->operation();

#if 0
        /* use this if you only want to process operations for a particular namespace.
         maybe add to cmd line parms or something fancier.
         */
        DbMessage ddd(m);
        if ( strncmp(ddd.getns(), "clusterstock", 12) != 0 ) {
            static int q;
            if ( ++q < 20 )
                out() << "TEMP skip " << ddd.getns() << endl;
            goto skip;
        }
#endif

        if ( m.data->operation() == dbQuery ) {
            // receivedQuery() does its own authorization processing.
            receivedQuery(dbresponse, m, ss, true);
        }
        else if ( m.data->operation() == dbMsg ) {
            ss << "msg ";
            char *p = m.data->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis() % 10000 <<
                     " long msg received, len:" << len <<
                     " ends with: " << p + len - 10 << endl;
            bool end = false; //strcmp("end", p) == 0;
            Message *resp = new Message();
            resp->setData(opReply, "i am fine");
            dbresponse.response = resp;
            dbresponse.responseTo = m.data->id;
            //dbMsgPort.reply(m, resp);
            if ( end )
                return false;
        }
        else if ( m.data->operation() == dbGetMore ) {
            // receivedQuery() does its own authorization processing.
            OPREAD;
            DEV log = true;
            ss << "getmore ";
            receivedGetMore(dbresponse, m, ss);
        }
        else {
            const char *ns = m.data->_data + 4;
            char cl[256];
            nsToClient(ns, cl);
            strncpy(currentOp.ns, ns, Namespace::MaxNsLen);
            AuthenticationInfo *ai = authInfo.get();
            if( !ai->isAuthorized(cl) ) { 
                uassert_nothrow("unauthorized");
            }
            else if ( m.data->operation() == dbInsert ) {
                OPWRITE;
                try {
                    ss << "insert ";
                    receivedInsert(m, ss);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion insert, continuing\n";
                    ss << " exception " + e.toString();
                }
            }
            else if ( m.data->operation() == dbUpdate ) {
                OPWRITE;
                try {
                    ss << "update ";
                    receivedUpdate(m, ss);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion update, continuing" << endl;
                    ss << " exception " + e.toString();
                }
            }
            else if ( m.data->operation() == dbDelete ) {
                OPWRITE;
                try {
                    ss << "remove ";
                    receivedDelete(m);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion receivedDelete, continuing" << endl;
                    ss << " exception " + e.toString();
                }
            }
            else if ( m.data->operation() == dbKillCursors ) {
                OPREAD;
                try {
                    log = true;
                    ss << "killcursors ";
                    receivedKillCursors(m);
                }
                catch ( AssertionException& e ) {
                    problem() << " Caught Assertion in kill cursors, continuing" << endl;
                    ss << " exception " + e.toString();
                }
            }
            else {
                out() << "    operation isn't supported: " << m.data->operation() << endl;
                currentOp.active = false;
                assert(false);
            }
        }
        ms = t.millis();
        log = log || (++ctr % 512 == 0 && !quiet);
        DEV log = true;
        if ( log || ms > 100 ) {
            ss << ' ' << t.millis() << "ms";
            out() << ss.str().c_str() << endl;
        }
        if ( database && database->profile >= 1 ) {
            if ( database->profile >= 2 || ms >= 100 ) {
                // profile it
                profile(ss.str().c_str()+20/*skip ts*/, ms);
            }
        }

        currentOp.active = false;
        return true;
    }

    void killCursors(int n, long long *ids);
    void receivedKillCursors(Message& m) {
        int *x = (int *) m.data->_data;
        x++; // reserved
        int n = *x++;
        assert( n >= 1 );
        if ( n > 2000 ) {
            problem() << "Assertion failure, receivedKillCursors, n=" << n << endl;
            assert( n < 30000 );
        }
        killCursors(n, (long long *) x);
    }

    void closeClient( const char *cl, const char *path ) {
        /* reset haveLogged in local.dbinfo */
        if ( string("local") != cl ) {
            DBInfo i(cl);
            i.dbDropped();
        }

        /* important: kill all open cursors on the database */
        string prefix(cl);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::drop( prefix.c_str() );

        eraseDatabase( cl, path );
        delete database; // closes files
        database = 0;
    }

    void receivedUpdate(Message& m, stringstream& ss) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        setClient(ns);
        //if( database->profile )
        ss << ns << ' ';
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        assert( d.moreJSObjs() );
        assert( query.objsize() < m.data->dataLen() );
        BSONObj toupdate = d.nextJsObj();

        assert( toupdate.objsize() < m.data->dataLen() );
        assert( query.objsize() + toupdate.objsize() < m.data->dataLen() );
        updateObjects(ns, toupdate, query, flags & 1, ss);
    }

    void receivedDelete(Message& m) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        setClient(ns);
        int flags = d.pullInt();
        bool justOne = flags & 1;
        assert( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();
        BSONObj deletedId = emptyObj;
        deleteObjects(ns, pattern, justOne, &deletedId);
        if ( justOne ) {
            if ( deletedId.isEmpty() ) {
                problem() << "deleted object without id, not logging" << endl;
            } else {
                logOp("d", ns, deletedId, 0, &justOne);
            }
        } else
            logOp("d", ns, pattern, 0, &justOne);            
    }

    void receivedQuery(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss, bool logit) {
        MSGID responseTo = m.data->id;

        DbMessage d(m);
        QueryMessage q(d);
        QueryResult* msgdata;

        try {
            /* note these are logged BEFORE authentication -- which is sort of ok */
            if ( opLogging && logit ) {
                if ( strstr(q.ns, ".$cmd") ) {
                    /* $cmd queries are "commands" and usually best treated as write operations */
                    OPWRITE;
                }
                else {
                    OPREAD;
                }
            }

            setClient( q.ns );
            strncpy(currentOp.ns, q.ns, Namespace::MaxNsLen);
            msgdata = runQuery(m, ss ).release();
        }
        catch ( AssertionException& e ) {
            ss << " exception ";
            LOGSOME problem() << " Caught Assertion in runQuery ns:" << q.ns << ' ' << e.toString() << '\n';
            log() << "  ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << '\n';
            if ( q.query.valid() )
                log() << "  query:" << q.query.toString() << endl;
            else
                log() << "  query object is not valid!" << endl;

            BSONObjBuilder err;
            err.append("$err", e.msg.empty() ? "assertion during query" : e.msg);
            BSONObj errObj = err.done();

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.append((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->resultFlags() = QueryResult::ResultFlag_ErrSet;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;

        }
        Message *resp = new Message();
        resp->setData(msgdata, true); // transport will free
        dbresponse.response = resp;
        dbresponse.responseTo = responseTo;
        if ( database ) {
            if ( database->profile )
                ss << " bytes:" << resp->data->dataLen();
        }
        else {
            if ( strstr(q.ns, "$cmd") == 0 ) // (this condition is normal for $cmd dropDatabase)
                log() << "ERROR: receiveQuery: database is null; ns=" << q.ns << endl;
        }
    }

    QueryResult* emptyMoreResult(long long);

    void receivedGetMore(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss) {
        DbMessage d(m);
        const char *ns = d.getns();
        ss << ns;
        setClient(ns);
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();
        ss << " cid:" << cursorid;
        ss << " ntoreturn:" << ntoreturn;
        QueryResult* msgdata;
        try {
            AuthenticationInfo *ai = authInfo.get();
            uassert("unauthorized", ai->isAuthorized(database->name.c_str()));
            msgdata = getMore(ns, ntoreturn, cursorid);
        }
        catch ( AssertionException& e ) {
            ss << " exception " + e.toString();
            msgdata = emptyMoreResult(cursorid);
        }
        Message *resp = new Message();
        resp->setData(msgdata, true);
        ss << " bytes:" << resp->data->dataLen();
        ss << " nreturned:" << msgdata->nReturned;
        dbresponse.response = resp;
        dbresponse.responseTo = m.data->id;
        //dbMsgPort.reply(m, resp);
    }

    void receivedInsert(Message& m, stringstream& ss) {
        DbMessage d(m);
		const char *ns = d.getns();
		assert(*ns);
		setClient(ns);
		ss << ns;
		
        while ( d.moreJSObjs() ) {
            BSONObj js = d.nextJsObj();

            theDataFileMgr.insert(ns, js);
            logOp("i", ns, js);
        }
    }

    extern int callDepth;

    class JniMessagingPort : public AbstractMessagingPort {
    public:
        JniMessagingPort(Message& _container) : container(_container) { }
        void reply(Message& received, Message& response, MSGID) {
            container = response;
        }
        void reply(Message& received, Message& response) {
            container = response;
        }
        Message & container;
    };

    /* a call from java/js to the database locally.

         m - inbound message
         out - outbound message, if there is any, will be set here.
               if there is one, out.data will be non-null on return.
    		   The out.data buffer will automatically clean up when out
    		   goes out of scope (out.freeIt==true)

       note we should already be in the mutex lock from connThread() at this point.
    */
    void jniCallback(Message& m, Message& out)
    {
		/* we should be in the same thread as the original request, so authInfo should be available. */
		AuthenticationInfo *ai = authInfo.get();
		massert("no authInfo in eval", ai);
        
        Database *clientOld = database;

        JniMessagingPort jmp(out);
        callDepth++;
        int curOpOld = curOp;

        try {

            stringstream ss;
            char buf[64];
            time_t_to_String(time(0), buf);
            buf[20] = 0; // don't want the year
            ss << buf << " dbjs ";

            {
                Timer t;

                bool log = false;
                curOp = m.data->operation();

                if ( m.data->operation() == dbQuery ) {
                    // on a query, the Message must have m.freeIt true so that the buffer data can be
                    // retained by cursors.  As freeIt is false, we make a copy here.
                    assert( m.data->len > 0 && m.data->len < 32000000 );
                    Message copy(malloc(m.data->len), true);
                    memcpy(copy.data, m.data, m.data->len);
                    DbResponse dbr;
                    receivedQuery(dbr, copy, ss, false);
                    jmp.reply(m, *dbr.response, dbr.responseTo);
                }
                else if ( m.data->operation() == dbInsert ) {
                    ss << "insert ";
                    receivedInsert(m, ss);
                }
                else if ( m.data->operation() == dbUpdate ) {
                    ss << "update ";
                    receivedUpdate(m, ss);
                }
                else if ( m.data->operation() == dbDelete ) {
                    ss << "remove ";
                    receivedDelete(m);
                }
                else if ( m.data->operation() == dbGetMore ) {
                    DEV log = true;
                    ss << "getmore ";
                    DbResponse dbr;
                    receivedGetMore(dbr, m, ss);
                    jmp.reply(m, *dbr.response, dbr.responseTo);
                }
                else if ( m.data->operation() == dbKillCursors ) {
                    try {
                        log = true;
                        ss << "killcursors ";
                        receivedKillCursors(m);
                    }
                    catch ( AssertionException& ) {
                        problem() << "Caught Assertion in kill cursors, continuing" << endl;
                        ss << " exception ";
                    }
                }
                else {
                    mongo::out() << "    jnicall: operation isn't supported: " << m.data->operation() << endl;
                    assert(false);
                }

                int ms = t.millis();
                log = log || ctr++ % 128 == 0;
                if ( log || ms > 100 ) {
                    ss << ' ' << t.millis() << "ms";
		    mongo::out() << ss.str().c_str() << endl;
                }
                if ( database && database->profile >= 1 ) {
                    if ( database->profile >= 2 || ms >= 100 ) {
                        // profile it
                        profile(ss.str().c_str()+20/*skip ts*/, ms);
                    }
                }
            }

        }
        catch ( AssertionException& ) {
            problem() << "Caught AssertionException in jniCall()" << endl;
        }

        curOp = curOpOld;
        callDepth--;

        if ( database != clientOld ) {
            database = clientOld;
            wassert(false);
        }
    }

    void getDatabaseNames( vector< string > &names ) {
        boost::filesystem::path path( dbpath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = i->leaf();
            if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                names.push_back( fileName.substr( 0, fileName.length() - 3 ) );
        }
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk ) {
        Authorizer a;
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
        assert( dbResponse.response );
        response = *dbResponse.response;
        return true;
    }

    void DBDirectClient::say( Message &toSend ) {
        Authorizer a;
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
    }

    DBDirectClient::AlwaysAuthorized DBDirectClient::Authorizer::always;

    void recCacheCloseAll();

    boost::mutex &exitMutex( *( new boost::mutex ) );
    bool firstExit = true;

    /* not using log() herein in case we are already locked */
    void dbexit(int rc, const char *why) {        
        {
            boostlock lk( exitMutex );
            if ( !firstExit ) {
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately" << endl;
                rawOut( ss.str() );
                ::exit( rc );                
            }
            firstExit = false;
        }
            
        stringstream ss;
        ss << "dbexit: " << why << endl;
        rawOut( ss.str() );
        
        stringstream ss2;
        flushOpLog( ss2 );
        rawOut( ss2.str() );

        /* must do this before unmapping mem or you may get a seg fault */
        closeAllSockets();

        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        rawOut( ss3.str() );

        // should we be locked here?  we aren't. might be ok as-is.
        recCacheCloseAll();

        rawOut( "dbexit: really exiting now\n" );
        ::exit(rc);
    }

} // namespace mongo
