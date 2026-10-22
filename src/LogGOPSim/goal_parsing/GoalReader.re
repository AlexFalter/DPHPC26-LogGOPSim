#include <map>
#include <unordered_map>
#include <unordered_set>
#include "goal_parsing/CustomFields.hpp"
#include <stdexcept>
#include <string>
#include <limits>
#include <utility>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "goal_parsing/GoalReader.hpp"

typedef unsigned int uint;
typedef unsigned char uchar;

#define	BSIZE	10240

#define	YYCTYPE		uchar
#define	YYCURSOR	cursor
#define	YYLIMIT		s->lim
#define	YYMARKER	s->ptr
#define	YYFILL(n)	{cursor = fill(s, cursor);}

typedef struct Scanner {
    FILE*		fd;
    uchar		*bot, *tok, *ptr, *cur, *lim, *top, *eof;
    uint		line;
    std::vector<traceschema::FieldValueInfo> custom_fields;
    std::unordered_map<std::string, std::size_t> custom_indices;
	int			rank;
	uint32_t 	num_ranks;
	Goal*		schedule;
	std::map<std::string, goalop_t> idtbl;
} Scanner;

typedef struct Item {
    std::vector<std::optional<traceschema::FieldValue>> custom_values;
    std::unordered_set<std::string> attributes;
    std::string attribute;
	char type;
	std::string label1;
	std::string label2;
	uint64_t size;
	uint64_t target;
	uint64_t tag;
	uint64_t cpu;
	uint64_t nic;
} Item;

enum OpTypes {
	Undefined,
	SendOp,
	RecvOp,
	LoclOp,
	StartDependency,
	Dependency
};

inline uint64_t add_number(unsigned char *s, unsigned char *e) {
    uint64_t number = 0;
    for (; s < e; ++s) {
        const auto digit = static_cast<uint64_t>(*s - '0');
        if (number > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            throw std::invalid_argument("Numeric literal exceeds uint64_t");
        number = number * 10 + digit;
    }
    return number;
}

static uint64_t checked_number(unsigned char* start, unsigned char* end,
                               uint64_t maximum, const char* field) {
    const auto number = add_number(start, end);
    if (number > maximum)
        throw std::invalid_argument(std::string("Legacy ") + field + " is out of range");
    return number;
}

static void check_operation(const Scanner* scanner, const Item* item) {
    if (!std::in_range<std::uint8_t>(item->cpu)) throw std::invalid_argument("Legacy cpu exceeds 255");
    if (!std::in_range<std::uint8_t>(item->nic)) throw std::invalid_argument("Legacy nic exceeds 255");
    if (!std::in_range<std::uint32_t>(item->tag)) throw std::invalid_argument("Legacy tag exceeds uint32_t");
    if (item->type != LoclOp && item->target != Node::any_source && item->target >= scanner->num_ranks)
        throw std::invalid_argument("Peer is outside num_ranks");
    if (item->type == SendOp && item->target == Node::any_source)
        throw std::invalid_argument("Send cannot use ANY_SOURCE");
}

static goalop_t retrieve_id(Scanner* s, const std::string& id) {
    const auto entry = s->idtbl.find(id);
    if (entry == s->idtbl.end())
        throw std::invalid_argument("A dependency references undefined label '" + id + "'");
    return entry->second;
}

static void process_item(Scanner* s, Item* item) {
    if (s->rank < 0) throw std::invalid_argument("Operation outside a rank block");
    if (item->type == SendOp || item->type == RecvOp || item->type == LoclOp)
        check_operation(s, item);
    goalop_t op;
    switch (item->type) {
    case SendOp:
        op = s->schedule->Send(item->target, item->size, item->tag, item->cpu, item->nic);
        break;
    case RecvOp:
        op = s->schedule->Recv(item->target, item->size, item->tag, item->cpu, item->nic);
        break;
    case LoclOp:
        op = s->schedule->Calc(item->size, item->cpu, item->nic);
        break;
    case StartDependency:
    case Dependency: {
        const auto dependent = retrieve_id(s, item->label1);
        const auto prerequisite = retrieve_id(s, item->label2);
        if (item->type == StartDependency) s->schedule->StartDependency(dependent, prerequisite);
        else s->schedule->Dependency(dependent, prerequisite);
        return;
    }
    default:
        throw std::invalid_argument("Attempt to add an undefined operation");
    }
    op->custom_values.reserve(item->custom_values.size());
    for (std::size_t index = 0; index < item->custom_values.size(); ++index) {
        if (!item->custom_values[index])
            throw std::invalid_argument("Missing custom field '" +
                std::get<std::string>(s->custom_fields[index].field) + "' on operation '" + item->label1 + "'");
        op->custom_values.push_back(*item->custom_values[index]);
    }
    if (!item->label1.empty() && !s->idtbl.emplace(item->label1, op).second)
        throw std::invalid_argument("Duplicate operation label '" + item->label1 + "'");
}

static std::string add_label(const uchar* start, const uchar* end) {
    return std::string(reinterpret_cast<const char*>(start), end - start);
}

static void set_attribute(Scanner* scanner, Item& item, std::string_view text) {
    const auto& name = item.attribute;
    if (!item.attributes.insert(name).second)
        throw std::invalid_argument("Duplicate attribute '" + name + "'");
    const auto parse_unsigned = [&] {
        return std::get<std::uint64_t>(traceschema::schema_helpers::parse_field_value(
            traceschema::FieldType::UnsignedInteger, text, name));
    };
    if (name == "cpu") item.cpu = parse_unsigned();
    else if (name == "nic") item.nic = parse_unsigned();
    else if (name == "tag" && item.type != LoclOp)
        item.tag = text == "-1" ? Node::any_tag : parse_unsigned();
    else {
        const auto found = scanner->custom_indices.find(name);
        if (found == scanner->custom_indices.end())
            throw std::invalid_argument("Unknown operation attribute '" + name + "'");
        const auto index = found->second;
        item.custom_values[index] = traceschema::schema_helpers::parse_field_value(
            scanner->custom_fields[index].type, text, name);
    }
}

static uchar* fill(Scanner* s, uchar* cursor) {
    if (s->eof) {
        if (cursor >= s->eof)
            throw std::invalid_argument("Unexpected end of GOAL input");
        return cursor;
    }
    const auto remaining = s->lim - s->tok;
    const auto cursor_offset = cursor - s->tok;
    const auto marker_offset = s->ptr >= s->tok ? s->ptr - s->tok : 0;
    if (s->tok != s->bot) memmove(s->bot, s->tok, remaining);
    s->tok = s->bot;
    s->lim = s->bot + remaining;
    if (s->top - s->lim < BSIZE) {
        auto* buffer = static_cast<uchar*>(realloc(s->bot, remaining + BSIZE + 1));
        if (!buffer) throw std::bad_alloc();
        s->bot = s->tok = buffer;
        s->lim = buffer + remaining;
        s->top = s->lim + BSIZE;
    }
    cursor = s->bot + cursor_offset;
    s->ptr = s->bot + marker_offset;
    const auto count = fread(s->lim, 1, BSIZE, s->fd);
    if (ferror(s->fd)) throw std::runtime_error("Failed reading GOAL input");
    s->lim += count;
    if (count < BSIZE) {
        *s->lim++ = '\n';
        s->eof = s->lim;
    }
    return cursor;
}

static int scan(Scanner *s) {

	uchar*& cursor = s->cur;
	Item item{};
	int state;

	for (;;) {

s_0:

	if (cursor == s->eof) {
		throw std::invalid_argument("Unexpected end of input: missing rank or closing bracket");
	}

	s->tok = cursor;
	state = 0;

	item = Item{};
    item.custom_values.resize(s->custom_fields.size());

/*!re2c
	re2c:indent:top = 2;

	NL			= "\r"? "\n" ;
	WS			= [ \t]+ ;
	RANK        = "rank" ;
	NUMRANKS	= "num_ranks" ;
	BROPEN      = "{" ;
	BRCLOSE     = "}" ;
	SEND		= "send" ;
	RECV		= "recv" ;
	CALC		= "calc" ;
	REQ			= "requires" ;
	IREQ		= "irequires" ;
	NIC			= "nic" ;
	CPU			= "cpu" ;
	TAG			= "tag" ;
	TO			= "to" ;
	FROM		= "from" ;
	COLON		= ":" ;
	BYTE		= "b" ;
	LCOMMENT    = "//" ;
	OCOMMENT    = "/*" ;
	CCOMMENT    = "*/" ;
	ANYSOURCE   = "-1" ;
	ANYTAG      = "-1" ;
	INT         = [0-9]+ ;
    SIGNED_INT  = "-"? INT ;
	IDENT       = [a-zA-Z][a-zA-Z0-9_]* ;
	ANY			= [^] ;

	WS			{ goto s_0; }
	SEND		{ item.type = SendOp;  goto s_2; }
	RECV		{ item.type = RecvOp;  goto s_2; }
	CALC		{ item.type = LoclOp;  goto s_3; }
	RANK        { goto s_20; }
	NUMRANKS	{ goto s_22; }
	IDENT		{ item.label1 = add_label(s->tok, cursor); goto s_1; }
	LCOMMENT    { goto s_23; }
	OCOMMENT    { goto s_24; }

	NL			{ s->line++; continue; }
	BRCLOSE     { if (s->rank == -1) goto s_err; int oldrank = s->rank; s->rank = -1; return oldrank; }
	ANY			{ goto s_err; }
*/

s_1:
	state =1;

/*!re2c
	WS			{ goto s_1; }
	COLON		{ goto s_4; }
	REQ			{ item.type = Dependency;       goto s_5; }
	IREQ		{ item.type = StartDependency;  goto s_5; }
	ANY			{ goto s_err; }
*/

s_2:
	state=2;

		s->tok = cursor;
/*!re2c
	WS			{ goto s_2; }
	INT			{ item.size = add_number(s->tok, cursor); goto s_10; }
	ANY			{ goto s_err; }
*/

s_3:
	state = 3;

		s->tok = cursor;
/*!re2c
	WS			{ goto s_3; }
	INT			{ item.size = add_number(s->tok, cursor); goto s_7; }
	ANY			{ goto s_err; }
*/

s_4:
	state = 4;

/*!re2c
	WS			{ goto s_4; }
	CALC		{ item.type = LoclOp; goto s_3; }
	SEND		{ item.type = SendOp; goto s_2; }
	RECV		{ item.type = RecvOp; goto s_2; }
	ANY			{ goto s_err; }
*/

s_5:
	state =5;

		s->tok = cursor;
/*!re2c
	WS			{ goto s_5; }
	IDENT		{ item.label2 = add_label(s->tok, cursor); goto s_6; }
	ANY			{ goto s_err; }
*/

s_6:
	state = 6;

/*!re2c
	WS			{ goto s_6; }
	NL			{ s->line++; process_item(s, &item); continue; }
	ANY			{ goto s_err; }
*/

s_7:
    goto s_attributes;

s_10:
	state=10;

/*!re2c
	WS			{ goto s_10; }
	BYTE		{ goto s_11; }
	ANY			{ goto s_err; }
*/

s_11:
	state = 11;

/*!re2c
	WS			{ goto s_11; }
	TO			{ if (item.type == SendOp) {goto s_12;} else {goto s_err;}; }
	FROM		{ if (item.type == RecvOp) {goto s_12;} else {goto s_err;}; }
	ANY			{ goto s_err; }
*/

s_12:
	state = 12;

	s->tok = cursor;

/*!re2c
	WS			{ goto s_12; }
	ANYSOURCE   {if (item.type == RecvOp) {item.target = Node::any_source; goto s_13;} else {goto s_err;}; }
	INT			{ item.target = add_number(s->tok, cursor); goto s_13; }
	ANY			{ goto s_err; }
*/

s_13:
    goto s_attributes;

s_attributes:
    state = 7;
    s->tok = cursor;
/*!re2c
    WS          { goto s_attributes; }
    IDENT       { item.attribute = add_label(s->tok, cursor); goto s_attribute_value; }
    NL          { s->line++; process_item(s, &item); continue; }
    ANY         { goto s_err; }
*/

s_attribute_value:
    state = 8;
    s->tok = cursor;
/*!re2c
    WS          { goto s_attribute_value; }
    SIGNED_INT  { set_attribute(s, item, add_label(s->tok, cursor)); goto s_attributes; }
    ANY         { goto s_err; }
*/

s_20:
	state = 20;

	s->tok = cursor;

/*!re2c
	WS			{ goto s_20; }
	INT			{ s->rank = checked_number(s->tok, cursor, INT32_MAX, "rank"); goto s_21; }
	ANY			{ goto s_err; }
*/

s_21:
	state = 21;

/*!re2c
	WS			{ goto s_21; }
	BROPEN		{ goto s_0; }
	ANY			{ goto s_err; }
*/

s_22:
	state = 22;

	s->tok = cursor;

/*!re2c
	WS			{ goto s_22; }
	INT			{ s->num_ranks = checked_number(s->tok, cursor, UINT32_MAX, "num_ranks"); goto s_0; }
	ANY			{ goto s_err; }
*/

s_23:
	state = 23;

/*!re2c
    NL          { s->line++; continue;  }
	ANY			{ goto s_23; }
*/

s_24:
	state = 24;

/*!re2c
	CCOMMENT    { continue; }
    NL          { s->line++; goto s_24;  }
	ANY			{ goto s_24; }
*/

s_err:

	fprintf(stderr, "Error in line %i:\n", s->line);

	uchar* nlbef = s->bot;
	uchar* nlaft = s->lim;
	// find the last newline before the error
	uchar* c = cursor - s->bot >= 2 ? cursor - 2 : s->bot;
	while (c>s->bot) {
		c--;
		if (*c == '\n') {
			nlbef = c;
			break;
		}
	}
	// find the next newline after the error
	c = cursor-2;
	while (c<s->lim) {
		c++;
		if (*c == '\n') {
			nlaft = c;
			break;
		}
	}

	for (uchar* c = nlbef+1; c<nlaft; c++) {
		fprintf(stderr, "%c", *c);
	}
	fprintf(stderr, "\n");

	for (int cnt=0; cnt<cursor-nlbef-1; cnt++) {
		fprintf(stderr, " ");
	}
	fprintf(stderr, "^\n");

	switch(state) {
		case 0:
			fprintf(stderr,"Expected: \"rank\", \"num_ranks\", \"calc\", \"send\", \"recv\", an identifier, \"}\" or \"\\n\"\n");
			break;
		case 1:
			fprintf(stderr,"Expected: \":\", \"requires\" or \"irequires\"\n");
			break;
		case 2:
			fprintf(stderr,"Expected: an integer\n");
			break;
		case 3:
			fprintf(stderr,"Expected: an integer\n");
			break;
		case 4:
			fprintf(stderr,"Expected: \"calc\", \"send\" or \"recv\"\n");
			break;
		case 5:
			fprintf(stderr,"Expected: an identifier\n");
			break;
		case 6:
			fprintf(stderr,"Expected: \"\\n\"\n");
			break;
		case 7:
			fprintf(stderr,"Expected: an attribute name or \"\\n\"\n");
			break;
		case 8:
			fprintf(stderr,"Expected: an integer\n");
			break;
		case 10:
			fprintf(stderr,"Expected: \"b\"\n");
			break;
		case 11:
			if (item.type == SendOp) fprintf(stderr,"Expected: \"to\"\n");
			else if (item.type == RecvOp) fprintf(stderr,"Expected: \"from\"\n");
			else fprintf(stderr,"Expected: \"to\" or \"from\"\n");
			break;
		case 12:
			fprintf(stderr,"Expected: an integer\n");
			break;
		case 13:
			fprintf(stderr,"Expected: \"tag\", \"cpu\", \"nic\" or \"\\n\"\n");
			break;
		case 20:
			fprintf(stderr,"Expected: an integer\n");
			break;
		case 21:
			fprintf(stderr,"Expected: \"{\"\n");
			break;
		case 22:
			fprintf(stderr,"Expected: an integer\n");
			break;
	}

	if (*(cursor-1) == '\n') {
		fprintf(stderr, "Instead the schedule contained: \"\\n\"\n");
	}
	else {
		fprintf(stderr, "Instead the schedule contained: \"%c\"\n", *(cursor-1));
	}

	throw std::invalid_argument("Invalid GOAL syntax at line " + std::to_string(s->line));

}
}

struct GoalReader::State {
    Scanner scanner{};
    std::uint32_t next_rank = 0;

    explicit State(const std::string& path, std::vector<traceschema::FieldValueInfo> custom_fields) {
        traceschema::validate_custom_fields(custom_fields);
        scanner.custom_fields = std::move(custom_fields);
        for (std::size_t index = 0; index < scanner.custom_fields.size(); ++index)
            scanner.custom_indices.emplace(std::get<std::string>(scanner.custom_fields[index].field), index);
        scanner.bot = static_cast<uchar*>(malloc(BSIZE + 1));
        if (!scanner.bot) throw std::bad_alloc();
        scanner.fd = fopen(path.c_str(), "r");
        if (!scanner.fd) {
            free(scanner.bot);
            throw std::runtime_error("Couldn't open input file " + path);
        }
        scanner.tok = scanner.ptr = scanner.cur = scanner.lim = scanner.bot;
        scanner.top = scanner.bot + BSIZE;
        scanner.rank = -1;
        scanner.line = 1;
    }

    ~State() {
        fclose(scanner.fd);
        free(scanner.bot);
    }
};

GoalReader::GoalReader(const std::string& path, std::vector<traceschema::FieldValueInfo> custom_fields)
    : state_(std::make_unique<State>(path, std::move(custom_fields))) {}
GoalReader::~GoalReader() = default;

std::unique_ptr<Goal> GoalReader::read_rank() {
    auto& s = state_->scanner;
    if (s.num_ranks != 0 && state_->next_rank == s.num_ranks) return nullptr;
    auto graph = std::make_unique<Goal>();
    s.schedule = graph.get();
    const auto rank = scan(&s);
    if (s.num_ranks == 0) throw std::invalid_argument("Number of ranks undefined");
    if (rank < 0 || static_cast<std::uint32_t>(rank) != state_->next_rank || static_cast<std::uint32_t>(rank) >= s.num_ranks)
        throw std::invalid_argument("Rank blocks must be numbered consecutively from zero");
    ++state_->next_rank;
    s.idtbl.clear();
    s.schedule = nullptr;
    graph->SetRank(rank);
    graph->SetNumRanks(s.num_ranks);
    return graph;
}
std::uint32_t GoalReader::get_num_ranks() const {
    return state_->scanner.num_ranks;
}
