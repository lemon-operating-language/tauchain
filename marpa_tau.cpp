/*
http://www.w3.org/2000/10/swap/grammar/bnf
http://www.w3.org/2000/10/swap/grammar/n3.n3
*/

#include <iostream>
#include <fstream>

#include "prover.h"
#include "json_object.h"
#include "cli.h"
#include "rdf.h"
#include "misc.h"
#include "jsonld.h"

#include "marpa_tau.h"

extern "C" {
#include "marpa.h"
#include "marpa_codes.c"
}

#include <boost/regex.hpp>
#include <boost/algorithm/string/predicate.hpp>




typedef Marpa_Symbol_ID sym;
typedef Marpa_Rule_ID rule;
typedef std::vector <sym> syms;
typedef std::pair <string::const_iterator, string::const_iterator> tokt;


class terminal {
public:
    nodeid thing;
    string name, regex_string;
    boost::wregex regex;

    terminal(nodeid thing_, string name_, string regex_) {
        name = name_;
        thing = thing_;
        regex_string = regex_;
        regex = boost::wregex(regex_);
    }
};

typedef std::shared_ptr <terminal> pterminal;

struct MarpaIris{
    nodeid iri(const std::string s)
    {
        return dict[mkiri(pstr(prefix + ws(s)))];
    }
    string prefix = L"http://idni.org/marpa#";
    const nodeid has_value = iri("has_value");
    const nodeid is_parse_of = iri("is_parse_of");
    const nodeid list_of = iri("list_of");
    const nodeid separator = iri("separator");
    const nodeid arg0 = iri("arg0");
    const nodeid arg1 = iri("arg1");
    const nodeid arg2 = iri("arg2");
};

MarpaIris *marpa = 0;

const string RDFS = L"http://www.w3.org/1999/02/22-rdf-syntax-ns#";
const string BNF  = L"http://www.w3.org/2000/10/swap/grammar/bnf#";

nodeid iri(const string prefix, const string s)
{
    return dict[mkiri(pstr(prefix + s))];
}

struct Marpa {
    Marpa_Grammar g;
    bool precomputed = false;
    // everything in the grammar is either a terminal, defined by a regex
    map <sym, pterminal> terminals;
    //, a "literal" - a simple string,
    map <sym, string> literals;
    ///or a rule
    map <rule, sym> rules;
    //done tracks both rules and terminals
    map <nodeid, sym> done;
    /*prvr is the one we are called from. we will use prvr2 for querying the grammar, so that prvr doesnt get messed up*/
    prover *prvr, *prvr2;
    // i use this for comment regex, comments are processed kinda specially so they dont have to pollute the grammar
    string whitespace = L"";
    const nodeid pcomma = dict[mkliteral(pstr(L","), 0, 0)];
    const nodeid rdfs_nil = iri(RDFS, L"nil");
    const nodeid rdfs_rest = iri(RDFS, L"rest");
    const nodeid rdfs_first = iri(RDFS, L"first");
    const nodeid bnf_matches = iri(BNF, L"matches");
    const nodeid bnf_document = iri(BNF, L"document");
    const nodeid bnf_whitespace = iri(BNF, L"whiteSpace");
    const nodeid bnf_zeroormore = iri(BNF, L"zeroOrMore");
    const nodeid bnf_mustBeOneSequence = iri(BNF, L"mustBeOneSequence");
    const nodeid bnf_commaSeparatedListOf = iri(BNF, L"commaSeparatedListOf");

    nodeid sym2resid(sym s) {
        for (auto it = done.begin(); it != done.end(); it++)
            if (it->second == s)
                return it->first;
        throw std::runtime_error("this sym isnt for a rule..this shouldnt happen");
    }

    nodeid rule2resid(rule r) {
        for (auto rr : rules)
            if (rr.first == r) {
                for (auto rrr: done)
                    if (rr.second == rrr.second)
                        return rrr.first;
            }
        throw std::runtime_error("and this shouldnt happen either");
    }

    string sym2str_(sym s) {
        if (literals.find(s) != literals.end())
            return L": \"" + literals[s] + L"\"";
        if (terminals.find(s) != terminals.end())
            return terminals[s]->name + L" - " + terminals[s]->regex_string;
        return *dict[sym2resid(s)].value;
    }

    string sym2str(sym s) {
        std::wstringstream sss;
        sss << L"(" << s << L")" << sym2str_(s);
        return sss.str();
    }

    string value(nodeid val) {
        return *dict[val].value;
    }

    ~Marpa() {
        marpa_g_unref(g);
    }

    Marpa(prover *prvr_, nodeid language) {
        /*init marpa*/
        if (marpa_check_version(MARPA_MAJOR_VERSION, MARPA_MINOR_VERSION, MARPA_MICRO_VERSION) != MARPA_ERR_NONE)
            throw std::runtime_error("marpa version...");
        Marpa_Config marpa_config;
        marpa_c_init(&marpa_config);
        g = marpa_g_new(&marpa_config);
        if (!g) {
            stringstream ssss("marpa_g_new: error ");
            ssss << marpa_c_error(&marpa_config, NULL);
            throw std::runtime_error(ssss.str());
        }
        if (marpa_g_force_valued(g) < 0)
            throw std::runtime_error("marpa_g_force_valued failed?!?!?");

        if (!marpa)marpa=new MarpaIris();

        prvr = prvr_;
        prvr2 = new prover(*prvr);
        /*bnf:whitespace is a property of bnf:language*/
        nodeid whitespace_ = prvr2->askn1o(language, bnf_whitespace);
        if (whitespace_) {
            whitespace = value(whitespace_);
            TRACE(dout << L"whitespace:" << whitespace <<std::endl);
        }
        /*so is bnf:document, the root rule*/
        nodeid root = prvr2->askn1o(language, bnf_document);
        sym start = add(root);
        start_symbol_set(start);
        delete prvr2;
    }

    //create marpa symbols and rules from grammar description in rdf
    sym add(nodeid thing) {

		//what we are adding
        string thingv = value(thing);
        TRACE(dout << "thingv:" << thingv << std::endl);

		//is it a rule or terminal thats been added or we started adding it already?
        if (done.find(thing) != done.end())
            return done[thing];

		//is it a literal?
        if ((dict[thing]._type == node::LITERAL) ||
                (dict[thing]._type == node::IRI && thingv == L".")) // crap: nquads parser bug workaround
        {
            //dout << "itsa str"<<std::endl;
            for (auto t: literals)
                if (t.second == thingv) // is it a done literal?
                    return t.first;

            sym symbol = symbol_new();
            //dout << "adding " << thingv << std::endl;

			//we add it to the literals table and are done with it.
            literals[symbol] = thingv;
            return symbol;
        }

        //dout << "adding " << thingv << std::endl;
        sym symbol = symbol_new_resid(thing);

		// is it a...
        nodeid bind;
        if ((bind = prvr2->askn1o(thing, bnf_matches))) {
            //dout << "terminal: " << thingv << std::endl;
            terminals[symbol] = pterminal(new terminal(thing, thingv, value(bind)));
            return symbol;
        }
        if ((bind = prvr2->askn1o(thing, bnf_mustBeOneSequence))) {
			// mustBeOneSequence is a list of lists
            std::vector <nodeid> lll = prvr2->get_list(bind);
            if (!bind)
                throw wruntime_error(L"mustBeOneSequence empty");

            for (auto l:lll) {
                syms rhs;
                std::vector <nodeid> seq = prvr2->get_list(l);
                for (auto rhs_item: seq)
                    rhs.push_back(add(rhs_item));
                rule_new(symbol, rhs);
            }
        }
        else if ((bind = prvr2->askn1o(thing, bnf_commaSeparatedListOf))) {
            seq_new(symbol, add(bind), add(pcomma), 0, MARPA_PROPER_SEPARATION);
        }
        else if ((bind = prvr2->askn1o(thing, marpa->list_of))) {
            auto sep = prvr2->askn1o(thing, marpa->separator);
            seq_new(symbol, add(bind), sep?add(sep):-1, 0, 0);
        }
        else if ((bind = prvr2->askn1o(thing, bnf_zeroormore))) {
            seq_new(symbol, add(bind), -1, 0, 0);
        }
        else if (thingv == L"http://www.w3.org/2000/10/swap/grammar/bnf#eof") { }//so what?
        else
            throw wruntime_error(L"whats " + thingv + L"?");

        TRACE(dout << "added sym " << symbol << std::endl);
        return symbol;
    }


    int check_int(int result) {
        if (result == -2)
            error();
        return result;
    }

    void check_null(void *result) {
        if (result == NULL)
            error();
    }

    void error_clear() {
        marpa_g_error_clear(g);
    }

    sym symbol_new() {
        return check_int(marpa_g_symbol_new(g));

    }

    sym symbol_new_resid(nodeid thing) {
        return done[thing] = symbol_new();

    }

    void rule_new(sym lhs, syms rhs) {
        TRACE(dout << sym2str(lhs) << L" ::= ");
        for (sym s: rhs)
            TRACE(dout << sym2str(s));
        TRACE(dout << std::endl);
        rules[check_int(marpa_g_rule_new(g, lhs, &rhs[0], rhs.size()))] = lhs;
        /*if (r == -2)
        {
            int e = marpa_g_error(g, NULL);
            if (e == MARPA_ERR_DUPLICATE_RULE)
            {
        */
    }

    void seq_new(sym lhs, sym rhs, sym separator, int min, int flags) {
        int r = marpa_g_sequence_new(g, lhs, rhs, separator, min, flags);
        if (r == -2) {
            int e = marpa_g_error(g, NULL);
            if (e == MARPA_ERR_DUPLICATE_RULE)
                dout << sym2str(lhs) << L" ::= sequence of " << sym2str(rhs) << std::endl;
        }
        rules[check_int(r)] = lhs;
    }


    void print_events() {

        int count = check_int(marpa_g_event_count(g));
        if (count > 0) {
            dout << count << " events" << std::endl;
            Marpa_Event e;
            for (int i = 0; i < count; i++) {
                int etype = check_int(marpa_g_event(g, &e, i));
                dout << L" " << etype << ", " << e.t_value << std::endl;
            }
        }

    }

    void start_symbol_set(sym s) {
        check_int(marpa_g_start_symbol_set(g, s));
    }

    void error() {

        int e = marpa_g_error(g, NULL);
        stringstream s;
        s << e << ":" << marpa_error_description[e].name << " - " << marpa_error_description[e].suggested;
        throw(std::runtime_error(s.str()));// + ": " + errors[e]));

    }


    bool is_ws(wchar_t x) {
        string wss = L"\n\r \t";

        for (auto ws: wss)
            if (x == ws)
                return true;
        return false;
    }


    int parse(const string inp, termid &raw) {
        if (!precomputed)
            check_int(marpa_g_precompute(g));

        print_events();

        TRACE(dout << "terminals:\n");
        for (auto t:terminals)
            TRACE(dout << "(" << t.first << ")" << t.second->name << ": " << t.second->regex << std::endl);

        TRACE(dout << "tokenizing..\n");

        std::vector <tokt> toks; // ranges of individual tokens within the input string
        toks.push_back(tokt(inp.end(), inp.end()));

        Marpa_Recognizer r = marpa_r_new(g);
        check_null(r);
        marpa_r_start_input(r);

        auto pos = inp.begin();

        std::vector <sym> expected;
        expected.resize(check_int(marpa_g_highest_symbol_id(g)));

        boost::wregex whitespace_regex = boost::wregex(whitespace);

        while (pos < inp.end()) {
            if (is_ws(*pos)) {
                pos++;
                continue;
            }

            boost::wsmatch what;

            if (whitespace.size() && regex_search(pos, inp.end(), what, whitespace_regex, boost::match_continuous)) {
                if (what.size()) {
                    int llll = what[0].length();
                    TRACE(dout << L"skipping " << llll << L" comment chars" << std::endl);
                    pos += llll;
                    continue;
                }
            }

            std::vector <sym> best_syms;
            size_t best_len = 0;
            expected.clear();
            int num_expected = check_int(marpa_r_terminals_expected(r, &expected[0]));
            for (int i = 0; i < num_expected; i++) {
                sym e = expected[i];
                if (literals.find(e) != literals.end()) {
                    if (boost::starts_with(string(pos, inp.end()), literals[e])) {
                        if (literals[e].size() > best_len) {
                            best_syms.clear();
                            best_syms.push_back(e);
                            best_len = literals[e].size();
                        }
                        else if (literals[e].size() == best_len)
                            best_syms.push_back(e);
                    }
                }
                else {
                    if (terminals.find(e) != terminals.end()) {
                        auto t = terminals[e];

                        if (!regex_search(pos, inp.end(), what, t->regex, boost::match_continuous)) continue;
                        if (what.empty()) continue;
                        size_t l = what.length();
                        if (l > best_len) {
                            best_len = l;
                            best_syms.clear();
                            best_syms.push_back(e);
                        }
                        else if (l == best_len)
                            best_syms.push_back(e);
                    }
                }
            }

            if (best_len) {
                if (best_syms.size() > 1) {
                    dout << L"cant decide between:" << std::endl;
                    for (auto ccc: best_syms)
                        dout << L" " << sym2str(ccc) << std::endl;
                }
                assert(best_syms.size());
                toks.push_back(tokt(pos, pos + best_len));
                TRACE(dout << std::distance(inp.begin(), pos) << L"-" << std::distance(inp.begin(), pos + best_len) <<
                L" \"" << string(pos, pos + best_len) << L"\" - " << sym2str(best_syms[0]) << std::endl);
                if (MARPA_ERR_UNEXPECTED_TOKEN_ID == check_int(marpa_r_alternative(r, best_syms[0], toks.size(), 1)))
                    return 0;
                check_int(marpa_r_earleme_complete(r));
                pos += best_len;
            }
            else {
                auto pre(pos);
                size_t charnum = 0;
                while (pre != inp.begin() && *pre != '\n') {
                    pre -= 1;
                    charnum++;
                }
                //pre -= 10;
                auto post(pos);
                while (post != inp.end() && *post != '\n')
                    post += 1;
                //post += 10;
                dout << L"at line " << 1 + std::count(inp.begin(), pos, '\n') << L", char " << charnum << L":" << std::endl;
                auto poss(pos);
                //if (poss != inp.begin()) poss--;
                dout << string(pre, poss) << L"<HERE>" << string(pos, post) << std::endl;
//                        dout << L"..\"" << string(pre, pos-1) << L"<HERE>" << string(pos, post) << L"\"..." << std::endl;


                dout << "expecting:" << std::endl;
                for (int i = 0; i < num_expected; i++) {
                    sym e = expected[i];
                    dout << sym2str(e) << std::endl;
                }


                return 0;
            }
        }

        TRACE(dout << "evaluating.." << std::endl);

        //marpa allows variously ordered lists of ambiguous parses, we just grab the default
        Marpa_Bocage b = marpa_b_new(r, -1);
        if (!b)
            return 0;
        Marpa_Order o = marpa_o_new(b);
        check_null(o);
        Marpa_Tree t = marpa_t_new(o);
        check_null(t);
        check_int(marpa_t_next(t));
        Marpa_Value v = marpa_v_new(t);
        check_null(v);

        //"valuator" loop. marpa tells us what rules with what child nodes or tokens were matched,
        //we build the value/parse/ast in the stack, bottom-up

        map <int, termid> stack;
        map <int, string> sexp;

        while (1) {
            Marpa_Step_Type st = check_int(marpa_v_step(v));
            print_events();
            if (st == MARPA_STEP_INACTIVE) break;
            switch (st) {
                case MARPA_STEP_TOKEN: {
                    sym symbol = marpa_v_symbol(v);
                    size_t token = marpa_v_token_value(v) - 1;
                    string token_value = string(toks[token].first, toks[token].second);
                    sexp[marpa_v_result(v)] = L"/*" + token_value + L"*/";
                    termid xx;
                    if (terminals.find(symbol) != terminals.end()) {
                        xx = prvr->make(mkbnode(gen_bnode_id()));
                        prvr->kb.add(prvr->make(bnf_matches, xx,  prvr->make(terminals[symbol]->thing)));
                        prvr->kb.add(prvr->make(marpa->has_value, xx, prvr->make(mkliteral(pstr(token_value), XSD_STRING, 0))));
                    }
                    else // it must be a literal
                        xx = prvr->make(mkliteral(pstr(token_value), 0, 0));
                    stack[marpa_v_result(v)] = xx;
                    break;
                }
                case MARPA_STEP_RULE: {
                    nodeid res = rule2resid(marpa_v_rule(v));
                    string sexp_str = L"\"" + value(res) + L"\":{ ";

                    std::list <termid> args;
                    for (int i = marpa_v_arg_0(v); i <= marpa_v_arg_n(v); i++) {
                        if (stack[i]) {
                            args.push_back(stack[i]);
                            sexp_str += sexp[i] + L" ";
                        }
                    }

                    termid xx;
                    // if its a sequence
                    if (check_int(marpa_g_sequence_min(g, marpa_v_rule(v))) != -1) {
                        xx = prvr->list2term_simple(args);
                    }
                    else {
                        xx = prvr->make(mkbnode(gen_bnode_id()));
                        //dout << L".xx: " << prvr->format(xx) << std::endl;
                        for (int i = 0; i <= marpa_v_arg_n(v) - marpa_v_arg_0(v); i++) {
                            termid arg = stack[marpa_v_arg_0(v) + i];

                            if (!arg) continue; // if it was nulled

                            sym arg_sym = check_int(marpa_g_rule_rhs(g, marpa_v_rule(v), i));

                            if (literals.find(arg_sym) == literals.end() )
                                prvr->kb.add(prvr->make(sym2resid(arg_sym), xx,  arg));

                            std::wstringstream arg_pred;
                            arg_pred << L"http://idni.org/marpa#arg" << i;

                            prvr->kb.add(prvr->make(mkiri(pstr(arg_pred.str())), xx, arg));
                        }
                    }

                    prvr->kb.add(prvr->make(marpa->is_parse_of, xx, prvr->make(res)));
                    //dout << L"xx: " << prvr->format(xx) << std::endl;
                    stack[marpa_v_result(v)] = xx;
                    sexp[marpa_v_result(v)] = sexp_str + L"} ";

                    break;
                }
                case MARPA_STEP_NULLING_SYMBOL:
                    sexp[marpa_v_result(v)] = L"0";
                    stack[marpa_v_result(v)] = 0;
                    break;
                default:
                    dout << marpa_step_type_description[st].name << std::endl;


            }
        }
        marpa_v_unref(v);
        if(check_int(marpa_t_next(t)) != -1)
            throw std::runtime_error("ambiguous parse");
        marpa_t_unref(t);
        marpa_o_unref(o);
        marpa_b_unref(b);

        TRACE(dout << L"{" << sexp[0] << L"}" << std::endl<< std::endl);

        raw = stack[0];
        TRACE(dout << L"result0: " << prvr->format(raw) << std::endl);
        return 2;
    }


};


class N3 {
public:

    nodeid uri(std::string s) {
        return dict[mkiri(pstr(ws("http://www.w3.org/2000/10/swap/grammar/n3#" + s)))];
    }

    nodeid n3symbol = uri("symbol");
    nodeid n3subject = uri("subject");
    nodeid n3object = uri("object");
    nodeid n3objecttail = uri("objecttail");
    nodeid n3explicituri = uri("explicituri");
    nodeid n3propertylist = uri("propertylist");
    nodeid n3propertylisttail = uri("propertylisttail");
    nodeid n3expression = uri("expression");
    nodeid n3pathitem = uri("pathitem");
    nodeid n3predicate = uri("predicate");
    //nodeid n3statement_with_dot = uri("statement_with_dot");
    nodeid n3statement = uri("statement");
    nodeid n3declaration = uri("declaration");
    nodeid n3simpleStatement = uri("simpleStatement");
    nodeid n3prefix = uri("prefix");
    nodeid n3qname = uri("qname");
    nodeid n3literal = uri("literal");
    nodeid n3numericliteral = uri("numericliteral");
    nodeid n3string = uri("string");
    nodeid n3boolean = uri("boolean");
    nodeid n3integer = uri("integer");
    //nodeid n3dtlang = uri("dtlang");
    nodeid n3quickvariable= uri("quickvariable");
    nodeid n3formulacontent = uri("formulacontent");

    prover *prvr;

    qdb &kb;
    qdb &query;

    qdb *dest;
    bool single_file_mode;

    string base;
    map<string, string> prefixes;

    string listid()
    {
        static int curid = 0;
        std::wstringstream ss;
        ss << L"_:list" << curid++;
        return ss.str();
    };

    prover::nodeids get_dotstyle_list(termid l) {
        std::list<nodeid> r;
        prvr->get_dotstyle_list(l, r);
        prover::nodeids rr;
        for (auto x:r)
            rr.push_back(x);
        return rr;
    }


    string get_value(nodeid n)
    {
        assert(n);
        nodeid v = q(n, marpa->has_value);
        assert(v);
        auto s = dict[v].value;
        assert(s);
        return *s;
    }

    N3(prover &prvr_, qdb &kb_, qdb &query_, bool single_file_mode_ = false):prvr(&prvr_), kb(kb_), query(query_), dest(&kb), single_file_mode(single_file_mode_)
    {
        if (!marpa)marpa=new MarpaIris();
    }

    nodeid q(nodeid s, nodeid p)
    {
        return prvr->askn1o(s, p);
    }

    void trim(string &s, string xxx)
    {
        assert(startsWith(s, xxx));
        assert(endsWith(s, xxx));
        s.erase(0, xxx.size());
        s.erase(s.size() - xxx.size());
    }

    pnode add_literal(nodeid x) {
        nodeid sss = q(x, n3string);
        assert (sss);
        nodeid v = q(sss, marpa->has_value);
        assert(v);
        string s = *dict[v].value;
        string triple = string(L"\"\"\"");

        if(startsWith(s, triple))
            trim(s, triple);
        else
            trim(s, L"\"");

        return mkliteral(pstr(s), 0, 0);
    }

    pnode add_numericliteral(nodeid x) {
        nodeid sss = q(x, n3integer);
        assert (sss);
        nodeid v = q(sss, marpa->has_value);
        assert(v);
        return mkliteral(dict[v].value, XSD_INTEGER, 0);
    }

    pnode add_boolean(nodeid x) {
        nodeid v = q(x, marpa->arg0);
        assert(v);

        auto s = *dict[v].value;
        s.erase(0, 1);
        return mkliteral(pstr(s), XSD_BOOLEAN, 0);
    }

    pnode add_symbol(nodeid x)
    {
        nodeid uri = q(x, n3explicituri);
        if (uri)
        {
            nodeid val = q(uri, marpa->has_value);
            assert(val);
            return mkiri(dict[val].value);
        }
        nodeid qname = q(x, n3qname);
        if (qname)
        {
            string v = get_value(qname);

            auto pos = v.find(L":");
            if (pos != string::npos)
            {
                string pref = string(v.begin(), v.begin() + pos);
                if (prefixes.find(pref) != prefixes.end())
                {
                    string rest = string(v.begin() + pos + 1, v.end());
                    v = prefixes[pref] + rest;
                }
            }
            else
            {
                v = base + v;
            }
            return mkiri(pstr(v));
        }
        assert(false);
    }

    pnode add_quickvariable(nodeid x)
    {
        nodeid v = q(x, marpa->has_value);
        assert(v);
        string s = *dict[v].value;
        //s[0] = '?';//workaround
        return mkiri(pstr(s));
    }

    pnode add_formulacontent(termid x)
    {
        auto graph = mkbnode(gen_bnode_id());
        add_statements(x, *graph->value);
        return graph;
    }

    pnode add_pathlist(termid/*TERMID - internalized list*/x)
    {
        std::list<pnode> r;
        if(x) {
            prover::nodeids items = get_dotstyle_list(x);
            for (auto i:items)
                r.push_back(add_expression(i));
        }
        auto id = listid();
        dest->second[id] = r;
        return mkbnode(pstr(id));
    }

    pnode add_pathitem(nodeid pi)
    {
        nodeid sym = q(pi, n3symbol);
        if (sym)
            return add_symbol(sym);
        termid f = prvr->askt1o(pi, n3formulacontent);
        if (f)
            return add_formulacontent(f);
        nodeid qv = q(pi, n3quickvariable);
        if (qv)
            return add_quickvariable(qv);
        nodeid nl = q(pi, n3numericliteral);
        if (nl)
            return add_numericliteral(nl);
        nodeid lit = q(pi, n3literal);
        if (lit)
            return add_literal(lit);

        //( "[" propertylist "]"  )

        nodeid l = q(pi, marpa->arg0);
        if(l && *dict[l].value == L"(") {
            termid x = prvr->askt1o(pi, marpa->arg1);
            return add_pathlist(x);
        }
        nodeid b = q(pi, n3boolean);
        if (b)
            return add_boolean(b);
        assert(false);

    }

    pnode add_expression(nodeid e)
    {
        nodeid sepi = q(e, n3pathitem);
        assert(sepi);
        //todo pathtail
        return add_pathitem(sepi);
    }

    void add_simpleStatement(nodeid sim, string graph)
    {
        assert(sim);
        TRACE(dout << "   " << sim << "   ");
        nodeid subj = q(sim, n3subject);
        assert(subj);
        nodeid se   = q(subj, n3expression);
        assert(se);
        nodeid sepi = q(se, n3pathitem);
        assert(sepi);
        pnode subject = add_pathitem(sepi);


        nodeid sspl = q(sim, n3propertylist);
        if (sspl) {
            nodeid prop = sspl;
            prover::nodeids props;
            while (prop) {
                props.push_back(prop);
                prop = q(prop, n3propertylisttail);
                if (prop)
                    prop = q(prop, n3propertylist);
            }
            TRACE(dout << "props:" << props.size());
            //now we have property lists in a list
            for (auto prop:props) {

                pnode predicate;
                bool reverse = false;

                nodeid pred = q(prop, n3predicate);
                nodeid pe = q(pred, n3expression);

                //get string value of first item
                nodeid i0 = q(pred, marpa->arg0);
                string i0v = *dict[i0].value; //?

                if (pe) { // if this predicate contains an expression
                    predicate = add_expression(pe);
                    /*      ( expression )
                        ( "@has" expression )
                      ( "@is" expression "@of" )*/
                    if (i0v == L"@is")
                        reverse = true;
                }
                else {
                    string ps;
                    if (i0v == L"@a")
                        ps = L"http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
                    else if (i0v == L"=")
                        ps = L"http://www.w3.org/2002/07/owl#sameAs";
                    else if (i0v == L"=>")
                        ps = L"http://www.w3.org/2000/10/swap/log#implies";
                    else if (i0v == L"<=") {
                        ps = L"http://www.w3.org/2000/10/swap/log#implies";
                        reverse = true;
                    }
                    else
                        throw std::runtime_error("this shouldnt happen");
                    predicate = mkiri(pstr(ps));
                }

                /* objects */
                std::vector<pnode> objs;
                nodeid t = prop;
                while (t) {
                    nodeid ob = q(t, n3object);
                    if(!ob)break;
                    nodeid oe = q(ob, n3expression);
                    objs.push_back(add_expression(oe));
                    t = q(t, n3objecttail);
                }

                for (auto object: objs) {
                    pnode s, o;
                    if (reverse) {
                        s = object;
                        o = subject;
                    }
                    else {
                        o = object;
                        s = subject;
                    }
                    if(dest->first.find(graph) == dest->first.end())
                        dest->first[graph] = mk_qlist();
                    dest->first[graph]->push_back(make_shared<quad>(s, predicate, o, graph));
                }
            }
        }
        else if (single_file_mode && *subject->value == L"fin" && graph == L"@default") {
            single_file_mode = false;//ignore further fins
            dout <<  kb.first.size() << " rules loaded." << std::endl;
            dest = &query;
        }
    }

    void add_statements(termid list, string graph) {
        prover::nodeids statements = get_dotstyle_list(list);
        TRACE(dout << std::endl << graph << ":" << std::endl);

        for (auto s: statements) {
            if (prvr->ask(s, marpa->is_parse_of, n3statement)) {
                nodeid sim = q(s, n3simpleStatement);
                if (sim)
                    add_simpleStatement(sim, graph);
            }else if (prvr->ask(s, marpa->is_parse_of, n3declaration)) {
                nodeid decl = q(s, n3declaration);
                nodeid a0 = q(decl, marpa->arg0);
                assert(a0);
                if(*dict[a0].value == L"@prefix")
                {
                    nodeid p = q(decl, n3prefix);
                    nodeid uri = q(decl, n3explicituri);
                    string uri_s = get_value(uri);
                    string p_s = get_value(p);
                    prefixes[p_s] = uri_s;
                }


            }

        }
    }
};










int parse_natural3(qdb &kb, qdb &q, std::wistream &f, string base)
{
    setproc(L"N3");
    std::string gfn = "n3-grammar.nq";
    std::wifstream gf(gfn);
    if (!gf.is_open())
		throw std::runtime_error("couldnt open file \"" + gfn + "\"");

    qdb gkb;
    readqdb(gkb, gf);

    static prover grmr(gkb);
    TRACE(dout << "grammar loaded."<<std::endl);
    static Marpa* parser = Marpa(&grmr, dict[mkiri(pstr(L"http://www.w3.org/2000/10/swap/grammar/n3#language"))]);

    string in = load_file(f);
    termid raw;
    int success = parser->parse(in, raw);

    if (success == 2)
{
TRACE(dout << std::endl << std::endl << "grmr:" << std::endl << grmr.formatkb());

if (!raw)
return 0;

TRACE(dout << "retrieving results." << std::endl);

N3 n3(grmr, kb, q, true);
n3.base = base;
n3.
add_statements(raw,
L"@default");
}

    return success;
}



/*builtins*/

void *marpa_parser(prover *p, nodeid language) {
    return (void*)new Marpa(p, language);
}

termid marpa_parse(void* marpa, string input) {
    Marpa *m = (Marpa *)marpa;
    termid result = m->parse(input);
    return result;
}

string load_file(std::ifstream &f) {
    std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return ws(str);
}




/*
resources

https://github.com/jeffreykegler/libmarpa/blob/master/test/simple/rule1.c#L18
https://github.com/jeffreykegler/Marpa--R2/issues/134#issuecomment-41091900
https://github.com/pstuifzand/marpa-cpp-rules/blob/master/marpa-cpp/marpa.hpp
 */
