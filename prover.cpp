#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <iostream>
#include <ostream>
#include "misc.h"
#include <utility>
#include "json_object.h"
#include "prover.h"
#include <iterator>
#include <forward_list>
#include <boost/algorithm/string.hpp>
#include <dlfcn.h>
#ifdef with_marpa
#include "marpa_tau.h"
#include <fstream>
#endif

using namespace boost::algorithm;
int _indent = 0;

term::term() : p(0), s(0), o(0) {}

bool prover::euler_path(shared_ptr<proof>& _p) {
	setproc(L"euler_path");
	if (_p) return false;
	auto& ep = _p;
	proof& p = *_p;
	termid t = heads[p.rule];
	if (!t) return false;
	const term& rt = *t;
	if (!p.s.empty()) {
		while ((ep = ep->prev))
			if (ep->rule == p.rule && unify_ep(heads[ep->rule], ep->s, rt, p.s))
				{ TRACE(dout<<"Euler path detected"<<endl); return true; }
	} else while ((ep = ep->prev))
		if (ep->rule == p.rule && unify_ep(heads[ep->rule], ep->s, rt))
			{ TRACE(dout<<"Euler path detected"<<endl); return true; }
	return false;
}

termid prover::tmpvar() {
	static int last = 1;
	return make(mkiri(pstr(string(L"?__v")+_tostr(last++))),0,0);
}

termid prover::list_next(termid cons, proof& p) {
	if (!cons) return 0;
	setproc(L"list_next");
	termset ts;
	ts.push_back(make(rdfrest, cons, tmpvar()));
	do_query( ts, &p.s);
	if (e.find(rdfrest) == e.end()) return 0;
	termid r = 0;
	for (auto x : e[rdfrest])
		if (x.first->s == cons) {
			r = x.first->o;
			break;
		}
	TRACE(dout <<"current cons: " << format(cons)<< " next cons: " << format(r) << std::endl);
	return r;
}

termid prover::list_first(termid cons, proof& p) {
	if (!cons || cons->p == rdfnil) return 0;
	setproc(L"list_first");
	termset ts;
	ts.push_back(make(rdffirst, cons, tmpvar()));
	do_query( ts, &p.s);
	if (e.find(rdffirst) == e.end()) return 0;
	termid r = 0;
	for (auto x : e[rdffirst])
		if (x.first->s == cons) {
			r = x.first->o;
			break;
		}
	TRACE(dout<<"current cons: " << format(cons) << " next cons: " << format(r) << std::endl);
	return r;
}

uint64_t dlparam(const node& n) {
	uint64_t p = 0;
	if (!n.datatype || !n.datatype->size() || *n.datatype == *XSD_STRING) {
		p = (uint64_t)n.value->c_str();
	} else {
		const string &dt = *n.datatype, &v = *n.value;
		if (dt == *XSD_BOOLEAN) p = (lower(v) == L"true");
		else if (dt == *XSD_DOUBLE) {
			double d;
			d = std::stod(v);
			memcpy(&p, &d, 8);
		} else if (dt == *XSD_INTEGER)
			p = std::stol(v);
	}
	return p;
}

std::vector<termid> prover::get_list(termid head, proof* _p) {
	setproc(L"get_list");
	assert(_p);
	proof& p = *_p;
	termid t = list_first(head, p);
	std::vector<termid> r;
	TRACE(dout<<"get_list with "<<format(head));
	while (t) {
		r.push_back(t);
		head = list_next(head, p);
		t = list_first(head, p);
	}
//	e = e1;
	TRACE(dout<<" returned " << r.size() << " items: "; for (auto n : r) dout<<format(n)<<' '; dout << std::endl);
	return r;
}

void prover::get_dotstyle_list(termid id, std::list<nodeid> &list) {
	auto s = id->s;
	if (!s) return;
	list.push_back(s->p);
	get_dotstyle_list(id->o, list);
	return;
}

void* testfunc(void* p) {
	derr <<std::endl<< "***** Test func called ****** " << p << std::endl;
	return (void*)(pstr("testfunc_result")->c_str());
//	return 0;
}

int prover::builtin(termid id, shared_ptr<proof> p, queue_t& queue) {
	setproc(L"builtin");
	const term& t = *id;
	int r = -1;
	termid i0 = t.s ? EVALPS(t.s, p->s) : 0;
	termid i1 = t.o ? EVALPS(t.o, p->s) : 0;
	term r1, r2;
	const term *t0 = i0 ? &(r1=*(i0=EVALPS(t.s, p->s))) : 0;
	const term* t1 = i1 ? &(r2=*(i1=EVALPS(t.o, p->s))) : 0;
	TRACE(	dout<<"called with term " << format(id); 
		if (t0) dout << " subject = " << format(i0);
		if (t1) dout << " object = " << format(i1);
		dout << endl);
	if (t.p == GND) r = 1;
	else if (t.p == logequalTo)
		r = t0 && t1 && t0->p == t1->p ? 1 : 0;
	else if (t.p == lognotEqualTo)
		r = t0 && t1 && t0->p != t1->p ? 1 : 0;
	else if (t.p == rdffirst && t0 && t0->p == Dot && (t0->s || t0->o))
		r = unify(t0->s, p->s, t.o, p->s) ? 1 : -1;
		// returning -1 here because plz also look into the kb,
		// dont assume that if this builtin didnt succeed, the kb cant contain such fact
	else if (t.p == rdfrest && t0 && t0->p == Dot && (t0->s || t0->o))
		r = unify(t0->o, p->s, t.o, p->s) ? 1 : -1;
/*	else if (t.p == _dlopen) {
		if (get(t.o).p > 0) throw std::runtime_error("dlopen must be called with variable object.");
		std::vector<termid> params = get_list(t.s, *p);
		if (params.size() >= 2) {
			void* handle;
			try {
				string f = predstr(params[0]);
				if (f == L"0") handle = dlopen(0, std::stol(predstr(params[1])));
				else handle = dlopen(ws(f).c_str(), std::stol(predstr(params[1])));
				pnode n = mkliteral(tostr((uint64_t)handle), XSD_INTEGER, 0);
				subs[p->s][get(t.o).p] = make(dict.set(n), 0, 0);
				r = 1;
			} catch (std::exception ex) { derr << indent() << ex.what() <<std::endl; }
			catch (...) { derr << indent() << L"Unknown exception during dlopen" << std::endl; }
		}
	}
	else if (t.p == _dlerror) {
		if (get(t.o).p > 0) throw std::runtime_error("dlerror must be called with variable object.");
		auto err = dlerror();
		pnode n = mkliteral(err ? pstr(err) : pstr(L"NULL"), 0, 0);
		subs[p->s][get(t.o).p] = make(dict.set(n), 0, 0);
		r = 1;
	}
	else if (t.p == _dlsym) {
//		if (t1) throw std::runtime_error("dlsym must be called with variable object.");
		if (!t1) {
			std::vector<termid> params = get_list(t.s, *p);
			void* handle;
			if (params.size()) {
				try {
					handle = dlsym((void*)std::stol(predstr(params[0])), ws(predstr(params[1])).c_str());
					pnode n = mkliteral(tostr((uint64_t)handle), XSD_INTEGER, 0);
					p->s[t1->p] = make(dict.set(n), 0, 0);
					r = 1;
				} catch (std::exception ex) { derr << indent() << ex.what() <<std::endl; }
				catch (...) { derr << indent() << L"Unknown exception during dlopen" << std::endl; }
			}
		}
	}
	else if (t.p == _dlclose) {
//		if (t1->p > 0) throw std::runtime_error("dlclose must be called with variable object.");
		pnode n = mkliteral(tostr(ws(dlerror())), 0, 0);
		p->s[t1->p] = make(dict.set(mkliteral(tostr(dlclose((void*)std::stol(*dict[t.p].value))), XSD_INTEGER, 0)), 0, 0);
		r = 1;
	}
	else if (t.p == _invoke) {
		typedef void*(*fptr)(void*);
		auto params = get_list(t.s, *p);
		if (params.size() != 2) return -1;
		if (preddt(params[0]) != *XSD_INTEGER) return -1;
		fptr func = (fptr)std::stol(predstr(params[0]));
		void* res;
		if (params.size() == 1) {
			res = (*func)((void*)dlparam(dict[*get_list(params[1],*p).begin()]));
			pnode n = mkliteral(tostr((uint64_t)res), XSD_INTEGER, 0);
			p->s[get(t.o).p] = make(dict.set(n), 0, 0);
		}
		r = 1;
	}*/
	/*
	else if (t.p == rdfsType || t.p == A) { // {?P @has rdfs:domain ?C. ?S ?P ?O} => {?S a ?C}.
		termset ts(2);
		termid p = tmpvar();
		termid o = tmpvar();
		ts[0] = make(rdfsdomain, p, t.o);
		ts[1] = make(p, t.s, o);
		//queue.push(make_shared<proof>(nullptr, kb.add(make(A, t.s, t.o), ts), 0, p, subs(), 0, true));
	}
	*/
	else if (t.p == rdfsType && t0 && t0->p == rdfsResource)  //rdfs:Resource(?x)
		r = 1;
	else if ((
					 t.p == A // parser kludge
					 || t.p == rdfsType || t.p == rdfssubClassOf) && t.s && t.o) {
		//termset ts(2,0,*alloc);
		termset ts(2);
		termid va = tmpvar();
		ts[0] = make ( rdfssubClassOf, va, t.o );
		ts[1] = make ( A, t.s, va );
		queue.push(make_shared<proof>(nullptr, kb.add(make ( A, t.s, t.o ), ts), 0, p, subs()));
	}
	else if (t.p == rdfsType || t.p == A) { // {?P @has rdfs:domain ?C. ?S ?P ?O} => {?S a ?C}.
		subs s;
		termset ts(1);
		termid p = tmpvar();
		termid o = tmpvar();
		ts[0] = make(rdfsdomain, p, t.o);
		prover copy(*this); //(Does this copy correctly?)
		copy.do_query(ts, &s);
		if (copy.e.size()) {
			termid np = evaluate(*p, s);
			std::cout << "\n\nYAY!!\n\n";
			ts[0] = make(np, t.s, o);
			copy.e.clear();
			subs s;
			copy.do_query(ts, &s);
			if (copy.e.size() > 0) {
				std::cout << "\n\nYay even more\n\n";
				//TODO: correctly, so that subqery proof trace not opaque?
				return 1;
			}
		}
	}
	#ifdef with_marpa
	else if (t.p == marpa_parser_iri && t.s && t.o)
	/* ?X is a parser created from grammar */
	{
		if (t0->p > 0) throw std::runtime_error("must be called with variable subject.");
		void* handle = marpa_parser(this, t1->p, p);
		pnode n = mkliteral(tostr((uint64_t)handle), XSD_INTEGER, 0);
		p->s[t0->p] = make(dict.set(n), 0, 0);
		r = 1;
	}
	else if (t.p == file_contents_iri) {
		if (t0->p > 0) throw std::runtime_error("file_contents must be called with variable subject.");
		string fn = *dict[t0->p].value;
		std::string fnn = ws(fn);
	    std::ifstream f(fnn);
	    if (f.is_open())
		{
			p->s[t0->p] = make(mkliteral(pstr(load_file(f)), 0, 0));
			r = 1;
		}
	}
	else if (t.p == marpa_parse_iri) {
	/* ?X is a parse of (input with parser) */
		if (t0->p > 0) throw std::runtime_error("marpa_parse must be called with variable subject.");
		termid xxx = t1->s;
		termid xxx2 = t1->o;
		string input = *dict[xxx->p].value;
		string marpa = *dict[xxx2->p].value;
		termid result = marpa_parse((void*)std::stol(marpa), input);
		p->s[t0->p] = result;
		r = 1;
	}
	#endif
	if (r == 1) 
		queue.push([p, id, this](){
			shared_ptr<proof> r = make_shared<proof>(p, *p);
			r->btterm = EVALPS(id, p->s);
			++r->term_idx;
			r->src = p->src;
			return r;
		}());
	return r;
}

void prover::pushev(shared_ptr<proof> p) {
	termid t;
	for (auto r : bodies[p->rule]) {
		if (!(t = (EVALPS(r, p->s)))) continue;
		e[t->p].emplace_back(t, p->g(this));
		if (level > 10) dout << "proved: " << format(t) << endl;
	}
}

#define queuepush(x) { auto y = x; if (lastp) lastp->next = y; lastp = y; } termsub.clear(); ++src;

shared_ptr<prover::proof> prover::step(shared_ptr<proof> _p) {
	setproc(L"step");
	if (steps % 1000000 == 0) (dout << "step: " << steps << endl);
	++steps;
	if (euler_path(_p)) return _p->next;
	const proof& frame = *_p;
	TRACE(dout<<"popped frame: " << formatp(_p) << endl);
	auto body = bodies[frame.rule];
	size_t src = 0;
	// if we still have some terms in rule body to process
	if (frame.term_idx != body.size()) {
		termid t = body[frame.term_idx];
		MARPA(if (builtin(t, _p, queue) != -1) return frame.next);//????
#ifdef PREDVARS
		if (t->p < 0)//ISVAR
			for(auto rulelst: kb.r2id)
				step_in(src, rulelst.second, _p, t);
		else {
#else
		if ((rit = kb.r2id.find(t->p)) == kb.r2id.end()) return frame.next;
		step_in(src, rit.second, _p, t);
#endif
#ifdef PREDVARS
		}
#endif
	}
	else if (!frame.prev) gnd.push(_p);
	else {
		proof& ppr = *frame.prev;
		shared_ptr<proof> r = make_shared<proof>(_p, ppr);
		ruleid rl = frame.rule;
		r->src = ppr.src;
		unify(heads[rl], frame.s, bodies[r->rule][r->term_idx], r->s = ppr.s);
		++r->term_idx;
		step(r);
	}
	return frame.next;
}

#ifdef PREDVARS
void prover::step_in(size_t &src, ruleset::rulelist &candidates, shared_ptr<proof> _p, termid t)
{
	proof& frame = *_p;
	if (!frame.s.empty()) {
		for (auto rule : rit->second) {
			if (unify(t, frame.s, heads[rule], termsub))
				queuepush(make_shared<proof>(_p, rule, 0, _p, termsub, src));
		}
	}
	else for (auto rule : rit->second) {
		if (unify(t, heads[rule], termsub))
			queuepush(make_shared<proof>(_p, rule, 0, _p, termsub, src));
		}
}
#endif
prover::ground prover::proof::g(prover* p) const {
	if (!creator) return ground();
	ground r = creator->g(p);
	if (btterm) r.emplace_back(p->kb.add(btterm, termset()), subs());
	else if (creator->term_idx != p->bodies[creator->rule].size()) {
		if (p->bodies[rule].empty()) r.emplace_back(rule, subs());
	} else if (!p->bodies[creator->rule].empty()) r.emplace_back(creator->rule, creator->s);
	return r;	
}

termid prover::list2term_simple(std::list<termid>& l) {
	setproc(L"list2term_simple");
	termid t;
	if (l.empty())
		t = make(Dot, 0, 0);
	else {
		termid x = l.front();
		l.pop_front();
		t = make(Dot, x, list2term_simple(l));
	}
	TRACE(dout << format(t) << endl);
	return t;
}

termid prover::list2term(std::list<pnode>& l, const qdb& quads) {
	setproc(L"list2term");
	termid t;
	if (l.empty()) t = make(Dot, 0, 0);
	else {
		pnode x = l.front();
		l.pop_front();
		auto it = quads.second.find(*x->value);
		//item is not a list
		if (it == quads.second.end())
			t = make(Dot, make(dict.set(x), 0, 0), list2term(l, quads));
		//item is a list
		else {
			auto ll = it->second;
			t = make(Dot, list2term(ll, quads), list2term(l, quads));
		}
	}
	TRACE(dout << format(t) << endl);
	return t;
}

termid prover::quad2term(const quad& p, const qdb& quads) {
	setproc(L"quad2term");
	TRACE(dout<<L"called with: "<<p.tostring()<<endl);
	termid t, s, o;
	#ifndef with_marpa
	if (dict[p.pred] == rdffirst || dict[p.pred] == rdfrest) return 0;
	#endif
	auto it = quads.second.find(*p.subj->value);
	if (it != quads.second.end()) {
		auto l = it->second;
		s = list2term(l, quads);
	}
	else
		s = make(p.subj, 0, 0);
	if ((it = quads.second.find(*p.object->value)) != quads.second.end()) {
		auto l = it->second;
		o = list2term(l, quads);
	}
	else
		o = make(p.object, 0, 0);
	t = make(p.pred, s, o);
	TRACE(dout<<"quad: " << p.tostring() << " term: " << format(t) << endl);
	return t;
}

qlist merge ( const qdb& q ) {
	qlist r;
	for ( auto x : q.first ) for ( auto y : *x.second ) r.push_back ( y );
	return r;
}

prover::~prover() { }
prover::prover(const prover& q) : kb(q.kb), _terms(q._terms) { kb.p = this; } 

void prover::addrules(pquad q, qdb& quads) {
	setproc(L"addrules");
	TRACE(dout<<q->tostring()<<endl);
	const string &s = *q->subj->value, &p = *q->pred->value, &o = *q->object->value;
	termid t;
	TRACE(dout<<"called with " << q->tostring()<<endl);
	if (p == implication) {
		if (quads.first.find(o) == quads.first.end()) quads.first[o] = mk_qlist();
		for ( pquad y : *quads.first.at ( o ) ) {
			if ( quads.first.find ( s ) == quads.first.end() ) continue;
			termset ts = termset();
			for ( pquad z : *quads.first.at( s ) )
				if ((dict[z->pred] != rdffirst && 
					dict[z->pred] != rdfrest) &&
					(t = quad2term(*z, quads)))
					ts.push_back( t );
			if ((t = quad2term(*y, quads))) kb.add(t, ts);
		}
	}// else
	if ((t = quad2term(*q, quads))) kb.add(t, termset()); // remarking the 'else' is essential for consistency checker
}

prover::prover ( qdb qkb, bool check_consistency ) : kb(this) {
	auto it = qkb.first.find(str_default);
	if (it == qkb.first.end()) throw std::runtime_error("Error: @default graph is empty.");
	if (qkb.first.find(L"false") == qkb.first.end()) qkb.first[L"false"] = make_shared<qlist>();
	for ( pquad quad : *it->second ) addrules(quad, qkb);
	if (check_consistency && !consistency(qkb)) throw std::runtime_error("Error: inconsistent kb");
}

bool prover::consistency(const qdb& quads) {
	setproc(L"consistency");
	bool c = true;
	prover p(*this);
	termid t = p.make(mkiri(pimplication), p.tmpvar(), p.make(False, 0, 0));
	termset g = termset();
	g.push_back(t);
	p.query(g);
	auto ee = p.e;
	for (auto x : ee) for (auto y : x.second) {
		prover q(*this);
		g.clear();
		string s = *dict[y.first->s->p].value;
		if (s == L"GND") continue;
		TRACE(dout<<L"Trying to prove false context: " << s << endl);
		qdb qq;
		qq.first[L""] = quads.first.at(s);
		q.do_query(qq);
		if (q.e.size()) {
			derr << L"Inconsistency found: " << q.format(y.first) << L" is provable as true and false."<<endl;
			c = false;
		}
	}
	return c;
}

prover::termset prover::qdb2termset(const qdb &q_) {
	termset goal = termset();
	termid t;
	for (auto q : merge(q_))
		if (dict[q->pred] != rdffirst && //wat
			dict[q->pred] != rdfrest &&
			(t = quad2term(*q, q_)))
			goal.push_back(t);
	return goal;
}


void prover::query(const qdb& q_, subs * s) {
	const termset t = qdb2termset(q_);
	query(t, s);
}

void prover::do_query(const qdb& q_, subs * s) {
	termset t = qdb2termset(q_);
	do_query(t, s);
}

void prover::query(const termset& goal, subs * s) {
	TRACE(dout << KRED << L"Rules:\n" << formatkb() << endl << KGRN << "Query: " << format(goal) << KNRM << std::endl);
	auto duration = do_query(goal, s);
	TRACE(dout << KYEL << "Evidence:" << endl);
	printe();/* << ejson()->toString()*/ dout << KNRM;
	dout << "elapsed: " << duration << "ms steps: " << steps << " unifs: " << unifs << " evals: " << evals << endl;
}

void prover::unittest() {
	pnode x = mkiri(pstr(L"x"));
	pnode a = mkiri(pstr(L"?a"));
	qdb &kb = *new qdb, &q = *new qdb;
	kb.first[str_default] = mk_qlist();
	q.first[str_default] = mk_qlist();
	kb.first[str_default]->push_back(make_shared<quad>(x, x, x));
	q.first[str_default]->push_back(make_shared<quad>(a, x, x));
	prover &p = *new prover(kb, false);
//	subs s1, s2;
//	termid xx = p.make(x, p.make(x,0,0), p.make(x,0,0));
//	termid aa = p.make(a, p.make(x,0,0), p.make(x,0,0));
//	for (uint n = 0; n < 2; ++n) {
//		p.unify(xx , s1, aa, s2);
//		dout <<"s1: "<< s1.format() << "s2: "<< s2.format() << endl;
//	}
//	exit(0);
	p.query(q);
	delete &kb;
	delete &q;
	delete &p;
}

int prover::do_query(const termid goal, subs * s)
{
	termset query;
	query.emplace_back(goal);
	return do_query(query, s);
}

int prover::do_query(const termset& goal, subs * s) {
	setproc(L"do_query");
	shared_ptr<proof> p = make_shared<proof>(nullptr, kb.add(0, goal)), q;
	if (s) p->s = *s;
	queue.push(p);
	
	TRACE(dout << KGRN << "Query: " << format(goal) << KNRM << std::endl);
	{
		setproc(L"rules");
		TRACE(dout << KRED << L"Rules:\n" << formatkb() << endl << KGRN << "Query: " << format(goal) << KNRM << std::endl);
	}

#ifdef TIMER
	using namespace std;
	using namespace std::chrono;
	high_resolution_clock::time_point t1 = high_resolution_clock::now();
#endif
	lastp = p;
	while ((p = step(p)));
//	do {
//		q = queue.top();//.get();
//		queue.pop();
//		printq(queue);
//		step(q);
//	} while (!queue.empty());// && steps < 2e+7);
#ifdef TIMER
	high_resolution_clock::time_point t2 = high_resolution_clock::now();
	auto duration = duration_cast<microseconds>( t2 - t1 ).count();
#else
	int duration = 0;
#endif

	while (!gnd.empty()) { auto x = gnd.top(); gnd.pop(); pushev(x); }
	TRACE(dout << KMAG << "Evidence:" << endl;printe();/* << ejson()->toString()*/ dout << KNRM);
//	TRACE(dout << "elapsed: " << (duration / 1000.) << "ms steps: " << steps << " evaluations: " << evals << " unifications: " << unifs << endl);
	return duration/1000.;
	//for (auto x : gnd) pushev(x);
}

term::term(nodeid _p, termid _s, termid _o) : p(_p), s(_s), o(_o) {}

termid prover::make(pnode p, termid s, termid o) { 
	return make(dict.set(*p), s, o); 
}

termid prover::make(nodeid p, termid s, termid o) {
#ifdef DEBUG
	if (!p) throw 0;
#endif
//	if ( (_terms.terms.capacity() - _terms.terms.size() ) < _terms.terms.size() )
//		_terms.terms.reserve(2 * _terms.terms.size());
	if (!p) throw 0;
	if (!s != !o) throw 0;
	return _terms.add(p, s, o);
}

prover::ruleid prover::ruleset::add(termid t, const termset& ts) {
	setproc(L"ruleset::add");
	ruleid r =  _head.size();
	_head.push_back(t);
	_body.push_back(ts);
	r2id[t ? t->p : 0].push_back(r);
	return r;
}

prover::ruleid prover::ruleset::add(termid t) {
	termset ts = termset();
	return add(t, ts);
}


bool prover::ask(termid s, nodeid p, termid o) {
	return askt(s, p, o, 1).size();
}

bool prover::ask(nodeid s, nodeid p, nodeid o) {
	return askt(make(s, 0, 0), p, make(o, 0, 0), 1).size();
}

/*query, return termids*/
prover::termids prover::askt(termid s, nodeid p, termid o, size_t stop_at) {
	prover::termids r;
	assert(s);assert(p);assert(o);
	setproc(L"ask");

	termid question = make(p, s, o);
	termset query;
	query.emplace_back(question);

	subs dummy;
	do_query(query, &dummy);

	std::vector<termid> vars;
	if (s->p < 0) vars.push_back(s);
	if (o->p < 1) vars.push_back(o);
	//ISVAR

	for (auto ei  : e)
	{
		for (auto x: ei.second)
		{
			for (auto g: x.second)
			{
				subs s = g.second;
				for (auto var:vars) {
					if (s.at(var->p)) {
						auto v = s[var->p];
						TRACE(dout << " match:");
						TRACE(dout << v << " ");
						r.push_back(v);
						if (stop_at && r.size() >= stop_at) {
							e.clear();
							return r;
						}
					}
				}
			}
		}
	}

	e.clear();
	return r;
}

prover::nodeids prover::askn(termid s, nodeid p, termid o, size_t stop_at) {
	auto r = askt(s, p, o, stop_at);
	prover::nodeids rr;
	for (auto rrr:r)
		rr.push_back(rrr->p);
	return rr;
}

/*query for subjects*/
prover::nodeids prover::askns(nodeid p, nodeid o, size_t stop_at) {
    assert(p && o);
    auto ot = make(o);
    assert (ot);
    termid s_var = tmpvar();
	assert(s_var);
	return askn(s_var, p, ot, stop_at);
}

/*query for objects*/
prover::nodeids prover::askno(nodeid s, nodeid p, size_t stop_at) {
    assert(s && p);
    auto st = make(s);
    assert (st);
    termid o_var = tmpvar();
	assert(o_var);
	return askn(st, p, o_var, stop_at);
}

/*query for one object*/
nodeid prover::askn1o(nodeid s, nodeid p) {
    return force_one_n(askno(s, p, 1));
}

/*query for one subject*/
nodeid prover::askn1s(nodeid p, nodeid o) {
	return force_one_n(askns(p, o, 1));
}

/*query for one object term*/
termid prover::askt1o(nodeid s, nodeid p) {
    assert(s);
    assert(p);
    termid xxs = make(s);
	termid o_var = tmpvar();
    assert (xxs);
    assert(o_var);
    return force_one_t(askt(xxs, p, o_var, 1));
}

nodeid prover::force_one_n(nodeids r) {
    /*#ifdef debug
     * if (r.size() > 1)
    {
        std::wstringstream ss;
        ss << L"well, this is weird, more than one match:";
        for (auto xx: r)
            ss << xx << " ";
        throw wruntime_error(ss.str());
    }
     #endif*/
    if (r.size() == 0)
        return 0;
    else
		return r[0];
}

termid prover::force_one_t(termids r) {
    if (r.size() == 0)
        return 0;
    else
		return r[0];
}

/*get_list wrapper useful in marpa*/
prover::nodeids prover::get_list(nodeid head)
{
    auto r = get_list(make(head), nullptr);
    nodeids rr;
    for (auto rrr: r)
        rr.push_back(rrr->p);
    return rr;
}
