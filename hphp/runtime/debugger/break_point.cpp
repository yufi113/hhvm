/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/debugger/break_point.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/runtime/debugger/debugger_proxy.h"
#include "hphp/runtime/debugger/debugger_thrift_buffer.h"
#include "hphp/runtime/base/preg.h"
#include "hphp/runtime/base/execution_context.h"
#include "hphp/runtime/base/class_info.h"
#include "hphp/runtime/base/stat_cache.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/base/comparisons.h"

namespace HPHP { namespace Eval {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(debugger);

int InterruptSite::getFileLen() const {
  TRACE(2, "InterruptSite::getFileLen\n");
  if (m_file.empty()) {
    getFile();
  }
  return m_file.size();
}

std::string InterruptSite::desc() const {
  TRACE(2, "InterruptSite::desc\n");
  string ret;
  if (m_error.isNull()) {
    ret = "Break";
  } else if (m_error.isObject()) {
    ret = "Exception thrown";
  } else {
    ret = "Error occurred";
  }

  const char *cls = getClass();
  const char *func = getFunction();
  if (func && *func) {
    ret += " at ";
    if (cls && *cls) {
      ret += cls;
      ret += "::";
    }
    ret += func;
    ret += "()";
  }

  string file = getFile();
  int line0 = getLine0();
  if (line0) {
    ret += " on line " + lexical_cast<string>(line0);
    if (!file.empty()) {
      ret += " of " + file;
    }
  } else if (!file.empty()) {
    ret += " in " + file;
  }

  return ret;
}

InterruptSite::InterruptSite(bool hardBreakPoint, CVarRef error)
    : m_error(error), m_class(nullptr), m_function(nullptr),
      m_file((StringData*)nullptr),
      m_line0(0), m_char0(0), m_line1(0), m_char1(0),
      m_offset(InvalidAbsoluteOffset), m_unit(nullptr), m_valid(false),
      m_funcEntry(false) {
  TRACE(2, "InterruptSite::InterruptSite\n");
  Transl::VMRegAnchor _;
#define bail_on(c) if (c) { return; }
  VMExecutionContext* context = g_vmContext;
  ActRec *fp = context->getFP();
  bail_on(!fp);
  if (hardBreakPoint && fp->skipFrame()) {
    // for hard breakpoint, the fp is for an extension function,
    // so we need to construct the site on the caller
    fp = context->getPrevVMState(fp, &m_offset);
    assert(fp);
    bail_on(!fp->m_func);
    m_unit = fp->m_func->unit();
    bail_on(!m_unit);
  } else {
    const uchar *pc = context->getPC();
    bail_on(!fp->m_func);
    m_unit = fp->m_func->unit();
    bail_on(!m_unit);
    m_offset = m_unit->offsetOf(pc);
    if (m_offset == fp->m_func->base()) {
      m_funcEntry = true;
    }
  }
  m_file = m_unit->filepath()->data();
  if (m_unit->getSourceLoc(m_offset, m_sourceLoc)) {
    m_line0 = m_sourceLoc.line0;
    m_char0 = m_sourceLoc.char0;
    m_line1 = m_sourceLoc.line1;
    m_char1 = m_sourceLoc.char1;
  }
  m_function = fp->m_func->name()->data();
  if (fp->m_func->preClass()) {
    m_class = fp->m_func->preClass()->name()->data();
  } else {
    m_class = "";
  }
#undef bail_on
  m_valid = true;
}

///////////////////////////////////////////////////////////////////////////////

const char *BreakPointInfo::ErrorClassName = "@";

const char *BreakPointInfo::GetInterruptName(InterruptType interrupt) {
  TRACE(2, "BreakPointInfo::GetInterruptName\n");
  switch (interrupt) {
    case RequestStarted: return "start of request";
    case RequestEnded:   return "end of request or start of psp";
    case PSPEnded:       return "end of psp";
    default:
      assert(false);
      break;
  }
  return nullptr;
}

BreakPointInfo::BreakPointInfo(bool regex, State state,
                               const std::string &file, int line)
    : m_index(0), m_state(state), m_valid(true),
      m_interruptType(BreakPointReached),
      m_file(file), m_line1(line), m_line2(line),
      m_regex(regex), m_check(false) {
  TRACE(2, "BreakPointInfo::BreakPointInfo..const std::string &file, int)\n");
  createIndex();
}

BreakPointInfo::BreakPointInfo(bool regex, State state,
                               InterruptType interrupt,
                               const std::string &url)
    : m_index(0), m_state(state), m_valid(true), m_interruptType(interrupt),
      m_line1(0), m_line2(0), m_url(url),
      m_regex(regex), m_check(false) {
  TRACE(2, "BreakPointInfo::BreakPointInfo..const std::string &url)\n");
  createIndex();
}

BreakPointInfo::BreakPointInfo(bool regex, State state,
                               InterruptType interrupt,
                               const std::string &exp,
                               const std::string &file)
    : m_index(0), m_state(state), m_valid(true), m_interruptType(interrupt),
      m_line1(0), m_line2(0),
      m_regex(regex), m_check(false) {
  TRACE(2, "BreakPointInfo::BreakPointInfo..const std::string &file)\n");
  assert(m_interruptType != ExceptionHandler); // Server-side only.
  if (m_interruptType == ExceptionThrown) {
    parseExceptionThrown(exp);
  } else {
    parseBreakPointReached(exp, file);
  }
  createIndex();
}

static int s_max_breakpoint_index = 0;
void BreakPointInfo::createIndex() {
  TRACE(2, "BreakPointInfo::createIndex\n");
  m_index = ++s_max_breakpoint_index;
}

BreakPointInfo::~BreakPointInfo() {
  TRACE(2, "BreakPointInfo::~BreakPointInfo\n");
  if (m_index && m_index == s_max_breakpoint_index) {
    --s_max_breakpoint_index;
  }
}

void BreakPointInfo::sendImpl(int version, DebuggerThriftBuffer &thrift) {
  TRACE(2, "BreakPointInfo::sendImpl\n");
  thrift.write((int8_t)m_state);
  if (version >= 1) thrift.write((int8_t)m_bindState);
  thrift.write((int8_t)m_interruptType);
  thrift.write(m_file);
  thrift.write(m_line1);
  thrift.write(m_line2);
  thrift.write(m_namespace);
  thrift.write(m_class);
  thrift.write(m_funcs);
  thrift.write(m_url);
  thrift.write(m_regex);
  thrift.write(m_check);
  thrift.write(m_clause);
  thrift.write(m_output);
  thrift.write(m_exceptionClass);
  thrift.write(m_exceptionObject);
}

void BreakPointInfo::recvImpl(int version, DebuggerThriftBuffer &thrift) {
  TRACE(2, "BreakPointInfo::recvImpl\n");
  int8_t tmp;
  thrift.read(tmp); m_state = (State)tmp;
  if (version >= 1) {
    thrift.read(tmp); m_bindState = (BindState)tmp;
  }
  thrift.read(tmp); m_interruptType = (InterruptType)tmp;
  thrift.read(m_file);
  thrift.read(m_line1);
  thrift.read(m_line2);
  thrift.read(m_namespace);
  thrift.read(m_class);
  thrift.read(m_funcs);
  thrift.read(m_url);
  thrift.read(m_regex);
  thrift.read(m_check);
  thrift.read(m_clause);
  thrift.read(m_output);
  thrift.read(m_exceptionClass);
  thrift.read(m_exceptionObject);
}

void BreakPointInfo::setClause(const std::string &clause, bool check) {
  TRACE(2, "BreakPointInfo::setClause\n");
  m_clause = clause;
  m_check = check;
}

void BreakPointInfo::changeBreakPointDepth(int stackDepth) {
  TRACE(2, "BreakPointInfo::changeBreakPointDepth\n");
  // if the breakpoint is equal or lower than the stack depth
  // delete it
  breakDepthStack.remove_if(
      std::bind2nd(std::greater_equal<int>(), stackDepth)
  );
}

void BreakPointInfo::unsetBreakable(int stackDepth) {
  TRACE(2, "BreakPointInfo::unsetBreakable\n");
  breakDepthStack.push_back(stackDepth);
}

void BreakPointInfo::setBreakable(int stackDepth) {
  TRACE(2, "BreakPointInfo::setBreakable\n");
  if (!breakDepthStack.empty() && breakDepthStack.back() == stackDepth) {
    breakDepthStack.pop_back();
  }
}

bool BreakPointInfo::breakable(int stackDepth) const {
  TRACE(2, "BreakPointInfo::breakable\n");
  if (!breakDepthStack.empty() && breakDepthStack.back() == stackDepth) {
    return false;
  } else {
    return true;
  }
}

void BreakPointInfo::toggle() {
  TRACE(2, "BreakPointInfo::toggle\n");
  switch (m_state) {
    case Always:   setState(Once);     break;
    case Once:     setState(Disabled); break;
    case Disabled: setState(Always);   break;
    default:
      assert(false);
      break;
  }
}

bool BreakPointInfo::valid() {
  TRACE(2, "BreakPointInfo::valid\n");
  if (m_valid) {
    switch (m_interruptType) {
      case BreakPointReached:
        if (!getFuncName().empty()) {
          if (!m_file.empty() || m_line1 != 0) {
            return false;
          }
        } else {
          if (m_file.empty() || m_line1 == 0 || m_line2 != m_line1) {
            return false;
          }
        }
        if (m_regex || m_funcs.size() > 1) {
          return false;
        }
        return (m_line1 && m_line2) || !m_file.empty() || !m_funcs.empty();
      case ExceptionThrown:
        return !m_class.empty();
      case RequestStarted:
      case RequestEnded:
      case PSPEnded:
        return true;
      default:
        break;
    }
  }
  return false;
}

bool BreakPointInfo::same(BreakPointInfoPtr bpi) {
  TRACE(2, "BreakPointInfo::same\n");
  return desc() == bpi->desc();
}

bool BreakPointInfo::match(InterruptType interrupt, InterruptSite &site) {
  TRACE(2, "BreakPointInfo::match\n");
  if (m_interruptType == interrupt) {
    switch (interrupt) {
      case RequestStarted:
      case RequestEnded:
      case PSPEnded:
        return
          checkUrl(site.url());
      case ExceptionThrown:
        return
          checkExceptionOrError(site.getError()) &&
          checkUrl(site.url()) && checkClause();
      case BreakPointReached:
      {
        bool match =
          Match(site.getFile(), site.getFileLen(), m_file, m_regex, false) &&
          checkLines(site.getLine0()) && checkStack(site) &&
          checkUrl(site.url()) && checkClause();

        if (!getFuncName().empty()) {
          // function entry breakpoint
          match = match && site.funcEntry();
        }
        return match;
      }
      default:
        break;
    }
  }
  return false;
}

std::string BreakPointInfo::state(bool padding) const {
  TRACE(2, "BreakPointInfo::state\n");
  switch (m_state) {
    case Always:   return padding ? "ALWAYS  " : "ALWAYS"  ;
    case Once:     return padding ? "ONCE    " : "ONCE"    ;
    case Disabled: return padding ? "DISABLED" : "DISABLED";
    default:
      assert(false);
      break;
  }
  return "";
}

std::string BreakPointInfo::regex(const std::string &name) const {
  TRACE(2, "BreakPointInfo::regex\n");
  if (m_regex) {
    return "regex{" + name + "}";
  }
  return name;
}

std::string BreakPointInfo::getNamespace() const {
  TRACE(2, "BreakPointInfo::getNamespace\n");
  if (!m_funcs.empty()) {
    return m_funcs[0]->m_namespace;
  }
  return "";
}

std::string BreakPointInfo::getClass() const {
  TRACE(2, "BreakPointInfo::getClass\n");
  if (!m_funcs.empty()) {
    return m_funcs[0]->m_class;
  }
  return "";
}

std::string BreakPointInfo::getFunction() const {
  TRACE(2, "BreakPointInfo::getFunction\n");
  if (!m_funcs.empty()) {
    return m_funcs[0]->m_function;
  }
  return "";
}

std::string BreakPointInfo::getFuncName() const {
  TRACE(2, "BreakPointInfo::getFuncName\n");
  if (!m_funcs.empty()) {
    return m_funcs[0]->getName();
  }
  return "";
}

std::string BreakPointInfo::site() const {
  TRACE(2, "BreakPointInfo::site\n");
  string ret;

  string preposition = "at ";
  if (!m_funcs.empty()) {
    ret = m_funcs[0]->site(preposition);
    for (unsigned int i = 1; i < m_funcs.size(); i++) {
      ret += " called by ";
      string tmp;
      ret += m_funcs[i]->site(tmp);
    }
  }

  if (!m_file.empty() || m_line1) {
    if (!ret.empty()) {
      ret += " ";
    } else {
      preposition = "";
    }
    if (m_line1) {
      ret += "on line " + lexical_cast<string>(m_line1);
      if (!m_file.empty()) {
        ret += " of " + m_file;
      }
    } else {
      ret += "in " + m_file;
    }
  }

  return preposition + ret;
}

std::string BreakPointInfo::descBreakPointReached() const {
  TRACE(2, "BreakPointInfo::descBreakPointReached\n");
  string ret;
  for (unsigned int i = 0; i < m_funcs.size(); i++) {
    ret += (i == 0 ? "upon entering " : " called by ");
    ret += m_funcs[i]->desc(this);
  }

  if (!m_file.empty() || m_line1 || m_line2) {
    if (!ret.empty()) {
      ret += " ";
    }
    if (m_line1 || m_line2) {
      if (m_line1 == m_line2) {
        ret += "on line " + lexical_cast<string>(m_line1);
      } else if (m_line2 == -1) {
        ret += "between line " + lexical_cast<string>(m_line1) + " and end";
      } else {
        ret += "between line " + lexical_cast<string>(m_line1) +
          " and line " + lexical_cast<string>(m_line2);
      }
      if (!m_file.empty()) {
        ret += " of " + regex(m_file);
      } else {
        ret += " of any file";
      }
    } else {
      ret += "on any lines in " + regex(m_file);
    }
  }
  return ret;
}

std::string BreakPointInfo::descExceptionThrown() const {
  TRACE(2, "BreakPointInfo::descExceptionThrown\n");
  string ret;
  if (!m_namespace.empty() || !m_class.empty()) {
    if (m_class == ErrorClassName) {
      ret = "right after an error";
    } else {
      ret = "right before throwing ";
      if (!m_class.empty()) {
        if (!m_namespace.empty()) {
          ret += regex(m_namespace) + "::";
        }
        ret += regex(m_class);
      } else {
        ret += "any exceptions in namespace " + regex(m_namespace);
      }
    }
  }
  return ret;
}

std::string BreakPointInfo::desc() const {
  TRACE(2, "BreakPointInfo::desc\n");
  string ret;
  switch (m_interruptType) {
    case BreakPointReached:
      ret = descBreakPointReached();
      break;
    case ExceptionThrown:
      ret = descExceptionThrown();
      break;
    default:
      ret = GetInterruptName((InterruptType)m_interruptType);
      break;
  }

  if (!m_url.empty()) {
    ret += " when request is " + regex(m_url);
  }

  if (!m_clause.empty()) {
    if (m_check) {
      ret += " if " + m_clause;
    } else {
      ret += " && " + m_clause;
    }
  }

  return ret;
}

void mangleXhpName(const std::string &source, std::string &target) {
  auto oldLen = source.length();
  size_t newLen = 0;
  size_t index = 0;
  for (; index < oldLen; index++) {
    auto ch = source[index];
    if (ch != ':' && ch != '-') continue;
    newLen = 4+index;
    break;
  }
  if (newLen == 0) {
    target = source;
    return;
  }
  for (; index < oldLen; index++) {
    if (source[index] == ':') newLen += 2; else newLen +=1;
  }
  target.clear();
  target.reserve(newLen);
  target.append("xhp_");
  for (index = 0; index < oldLen; index++) {
    auto ch = source[index];
    if (ch == '-') {
      target.push_back('_');
    } else if (ch == ':') {
      if (index > 0) {
        target.push_back('_');
        target.push_back('_');
      }
    } else {
      target.push_back(ch);
    }
  }
}

int32_t scanName(const std::string &str, int32_t offset) {
  auto len = str.length();
  assert(0 <= offset && offset < len);
  while (offset < len) {
    char ch = str[offset];
    if (ch == ':' || ch == '\\' || ch == ',' || ch == '(' || ch == '=' ||
        ch == '@') {
      if (offset+1 >= len) return offset;
      char ch1 = str[offset+1];
      if (ch == ':') {
        if (ch1 == ':' || ('0' <= ch1 && ch1 <= '9')) return offset;
      } else if (ch == '(') {
        if (ch1 == ')') return offset;
      } else if (ch == '=') {
        if (ch1 == '>') return offset;
      } else {
        assert (ch == '\\' || ch == ',' || ch == '@');
        return offset;
      }
    }
    offset++;
  }
  return offset;
}

int32_t scanNumber(const std::string &str, int32_t offset, int32_t& value) {
  value = 0;
  auto len = str.length();
  assert(0 <= offset && offset < len);
  while (offset < len) {
    char ch = str[offset];
    if (ch < '0' || ch > '9') return offset;
    value = value*10 + (ch - '0');
    offset++;
  }
  return offset;
}

int32_t BreakPointInfo::parseFileLocation(const std::string &str,
                                          int32_t offset) {
  auto len = str.length();
  assert(0 <= offset && offset < len);
  auto offset1 = scanNumber(str, offset, m_line1);
  if (offset1 == offset) return offset; //Did not find a number
  m_line2 = m_line1; //so that we always have a range
  if (offset1 >= len) return len; //Nothing follows the number
  auto ch = str[offset1];
  if (ch == '-') {
    if (offset1+1 >= len) return offset; //Invalid file location
    auto offset2 = scanNumber(str, offset1+1, m_line2);
    if (offset1+1 == offset2) return offset; //Invalid file location
    return offset2;
  }
  return offset1;
}

/* The parser accepts the following syntax, which harks back to pre VM days
   (all components are optional, as long as there is at least one component):

   {file location},{call}=>{call}()@{url}
   {call}=>{call}(),{file location}@{url}

   file location: {file}:{line1}-{line2}
   call: \{namespace}\{cls}::{func}

   Currently semantic checks will disallow expressions that specify
   both file locations and calls.
*/
void BreakPointInfo::parseBreakPointReached(const std::string &exp,
                                            const std::string &file) {
  TRACE(2, "BreakPointInfo::parseBreakPointReached\n");

  string name;
  auto len = exp.length();
  auto offset0 = 0;
  //Look for leading number by itself
  auto offset1 = scanNumber(exp, offset0, m_line1);
  if (offset1 == len) {
    m_line2 = m_line1;
    m_file = file;
    return;
  }
  // Skip over a leading backslash
  if (len > 0 && exp[0] == '\\') offset0++;
  offset1 = scanName(exp, offset0);
  // check that exp starts with a file or method name
  if (offset1 == offset0) goto returnInvalid;
  name = exp.substr(offset0, offset1-offset0);

  if (offset0 == 0) {
    // parse {file location} if appropriate
    if (offset1 < len && exp[offset1] == ',') {
      m_file = name;
      name.clear();
      offset1 += 1;
    } else if (offset1 < len-1 && exp[offset1] == ':' &&
               exp[offset1+1] != ':') {
      m_file = name;
      name.clear();
      offset1 += 1;
      auto offset2 = parseFileLocation(exp, offset1);
      // check for {file}:{something that is not a number}
      if (offset2 == offset1) goto returnInvalid;
      offset1 = offset2;
      if (offset1 >= len) return; // file location without anything else
      if (exp[offset1] == '@') goto parseUrl; // file location followed by url
      // check for {file}{ something other than @ or :}
      if (exp[offset1] != ',') goto returnInvalid;
      offset1 += 1;
    }
  }

  // parse {func}() or {func}=>{func}() or {func}=>{func}=>{func}() and so on
  while (true) {
    if (name.empty()) {
      if (len > offset1 && exp[offset1] == '\\') offset1++;
      auto offset2 = scanName(exp, offset1);
      // check for {something other than a name}
      if (offset2 == offset1) goto returnInvalid;
      name = exp.substr(offset1, offset2-offset1);
    }
    while (offset1 < len && exp[offset1] == '\\') {
      if (!m_namespace.empty()) m_namespace += "\\";
      m_namespace += name;
      offset1 += 1;
      auto offset2 = scanName(exp, offset1);
      // check for {namespace}\{something that is not a name}
      if (offset2 == offset1) goto returnInvalid;
      name = exp.substr(offset1, offset2-offset1);
      offset1 = offset2;
    }
    if (offset1 < len-1 && exp[offset1] == ':' && exp[offset1+1] == ':') {
      m_class = name;
      offset1 += 2;
      auto offset2 = scanName(exp, offset1);
      // check for {namespace}\{class}::{something that is not a name}
      if (offset2 == offset1) goto returnInvalid;
      name = exp.substr(offset1, offset2-offset1);
      offset1 = offset2;
    }
    // Now we have a namespace, class and func name.
    // The namespace only or the namespace and class might be empty.
    DFunctionInfoPtr pfunc(new DFunctionInfo());
    if (m_class.empty()) {
      if (m_namespace.empty())
        pfunc->m_function = name;
      else {
        // Yes this does seem beyond strange, but that is what the PHP parser
        // does when it sees a function declared inside a namespace, so we
        // too have to pretend there is no namespace here. At some point
        // the parser hack may have to go away. At that stage, this code
        // will have to change, as well as other parts of the debugger.
        pfunc->m_function = m_namespace + "\\" + name;
      }
    } else {
      mangleXhpName(m_class, pfunc->m_class);
      if (!m_namespace.empty()) {
        // Emulate parser hack. See longer comment above.
        pfunc->m_class = m_namespace + "\\" + pfunc->m_class;
      }
      pfunc->m_function = name;
    }
    m_funcs.insert(m_funcs.begin(), pfunc);
    m_namespace.clear();
    m_class.clear();
    name.clear();
    // If we are now at () we skip over it and terminate the loop
    if (offset1 < len && exp[offset1] == '(') {
      // check for {func}{(}{not )}
      if (offset1+1 >= len || exp[offset1+1] != ')') goto returnInvalid;
      offset1 += 2;
      break; // parsed the last (perhaps only) call in a function call chain
    }
    // If we are now at => we need to carry on with the loop
    if (offset1 < len-1 && exp[offset1] == '=' && exp[offset1+1] == '>') {
      offset1 += 2;
      continue;
    }
    goto returnInvalid; // {func calls}{not () or =>}
  }
  if (m_file.empty()) {
    if (offset1 < len && exp[offset1] == ',') {
      auto offset2 = scanName(exp, ++offset1);
      // check for {func calls}:{not a filename}
      if (offset2 == offset1) goto returnInvalid;
      m_file = exp.substr(offset1, offset2-offset1);
      offset1 = offset2;
      if (offset1 < len && exp[offset1] == ':') {
        offset2 = parseFileLocation(exp, offset1+1);
        // check for {file}:{something that is not a number}
        if (offset2 == offset2+1) goto returnInvalid;
        offset1 = offset2;
      }
    }
  }
parseUrl:
  if (offset1 < len-2 && exp[offset1] == '@') {
    offset1++;
    m_url = exp.substr(offset1, len-offset1);
  } else {
    // check for unparsed characters at end of exp
    if (offset1 != len) goto returnInvalid;
  }
  return;

returnInvalid:
  m_valid = false;
}

void BreakPointInfo::parseExceptionThrown(const std::string &exp) {
  TRACE(2, "BreakPointInfo::parseExceptionThrown\n");

  string name;
  auto len = exp.length();
  auto offset0 = 0;
  // Skip over a leading backslash
  if (len > 0 && exp[0] == '\\') offset0++;
  auto offset1 = scanName(exp, offset0);
  // check that exp starts with a name
  if (offset1 == offset0) goto returnInvalid;
  name = exp.substr(offset0, offset1-offset0);
  if (name.empty()) {
    if (len > offset1 && exp[offset1] == '\\') offset1++;
    auto offset2 = scanName(exp, offset1);
    // check for {something other than a name}
    if (offset2 == offset1) goto returnInvalid;
    name = exp.substr(offset1, offset2-offset1);
  }
  while (offset1 < len && exp[offset1] == '\\') {
    if (!m_namespace.empty()) m_namespace += "\\";
    m_namespace += name;
    offset1 += 1;
    auto offset2 = scanName(exp, offset1);
    // check for {namespace}\{something that is not a name}
    if (offset2 == offset1) goto returnInvalid;
    name = exp.substr(offset1, offset2-offset1);
    offset1 = offset2;
  }
  m_class = name;
  // Now we have a namespace and class name.
  // The namespace might be empty.
  mangleXhpName(m_class, m_class);
  if (m_class == "error") m_class = ErrorClassName;
  if (!m_namespace.empty()) {
    m_class = m_namespace + "\\" + m_class;
    m_namespace.clear();
  }
  if (offset1 < len-2 && exp[offset1] == '@') {
    offset1++;
    m_url = exp.substr(offset1, len-offset1);
  } else {
    // check for unparsed characters at end of exp
    if (offset1 != len) goto returnInvalid;
  }
  return;

returnInvalid:
  m_valid = false;
}

bool BreakPointInfo::MatchFile(const char *haystack, int haystack_len,
                               const std::string &needle) {
  TRACE(2, "BreakPointInfo::MatchFile(const char *haystack\n");
  int pos = haystack_len - needle.size();
  if ((pos == 0 || haystack[pos - 1] == '/') &&
      strcasecmp(haystack + pos, needle.c_str()) == 0) {
    return true;
  }
  if (strcasecmp(StatCache::realpath(needle.c_str()).c_str(), haystack)
      == 0) {
    return true;
  }
  return false;
}

bool BreakPointInfo::MatchFile(const std::string& file,
                               const std::string& fullPath,
                               const std::string& relPath) {
  TRACE(2, "BreakPointInfo::MatchFile(const std::string&\n");
  if (file == fullPath || file == relPath) {
    return true;
  }
  if (file.find('/') == std::string::npos &&
      file == fullPath.substr(fullPath.rfind('/') + 1)) {
    return true;
  }
  // file is possibly setup with a symlink in the path
  if (StatCache::realpath(file.c_str()) ==
      StatCache::realpath(fullPath.c_str())) {
    return true;
  }
  return false;
}

bool BreakPointInfo::MatchClass(const char *fcls, const std::string &bcls,
                                bool regex, const char *func) {
  TRACE(2, "BreakPointInfo::MatchClass\n");
  if (bcls.empty()) return true;
  if (!fcls || !*fcls) return false;
  if (regex || !func || !*func) {
    return Match(fcls, 0, bcls, true, true);
  }

  StackStringData sdBClsName(bcls.c_str());
  Class* clsB = Unit::lookupClass(&sdBClsName);
  StackStringData sdFClsName(fcls);
  Class* clsF = Unit::lookupClass(&sdFClsName);
  if (!clsB) return false;
  if (clsB == clsF) return true;
  StackStringData sdFuncName(func);
  Func* f = clsB->lookupMethod(&sdFuncName);
  if (!f) return false;
  return (f->baseCls() == clsF);
}

bool BreakPointInfo::Match(const char *haystack, int haystack_len,
                           const std::string &needle, bool regex, bool exact) {
  TRACE(2, "BreakPointInfo::Match\n");
  if (needle.empty()) {
    return true;
  }
  if (!haystack || !*haystack) {
    return false;
  }

  if (!regex) {
    if (exact) {
      return strcasecmp(haystack, needle.c_str()) == 0;
    }
    return MatchFile(haystack, haystack_len, needle);
  }

  Variant matches;
  Variant r = preg_match(String(needle.c_str(), needle.size(),
                                AttachLiteral),
                         String(haystack, haystack_len, AttachLiteral),
                         matches);
  return HPHP::same(r, 1);
}

bool BreakPointInfo::checkExceptionOrError(CVarRef e) {
  TRACE(2, "BreakPointInfo::checkException\n");
  assert(!e.isNull());
  if (e.isObject()) {
    if (m_regex) {
      return Match(m_class.c_str(), m_class.size(),
                   e.toObject()->o_getClassName().data(), true, false);
    }
    return e.instanceof(m_class.c_str());
  }
  return Match(m_class.c_str(), m_class.size(), ErrorClassName, m_regex,
               false);
}

bool BreakPointInfo::checkUrl(std::string &url) {
  TRACE(2, "BreakPointInfo::checkUrl\n");
  if (!m_url.empty()) {
    if (url.empty()) {
      url = "/";
      Transport *transport = g_context->getTransport();
      if (transport) {
        url += transport->getCommand();
      }
    }
    return Match(url.c_str(), url.size(), m_url, m_regex, false);
  }
  return true;
}

bool BreakPointInfo::checkLines(int line) {
  TRACE(2, "BreakPointInfo::checkLines\n");
  if (m_line1) {
    assert(m_line2 == -1 || m_line2 >= m_line1);
    return line >= m_line1 && (m_line2 == -1 || line <= m_line2);
  }
  return true;
}

bool BreakPointInfo::checkStack(InterruptSite &site) {
  TRACE(2, "BreakPointInfo::checkStack\n");
  if (m_funcs.empty()) return true;

  if (!Match(site.getNamespace(), 0, m_funcs[0]->m_namespace, m_regex, true) ||
      !Match(site.getFunction(),  0, m_funcs[0]->m_function,  m_regex, true) ||
      !MatchClass(site.getClass(), m_funcs[0]->m_class, m_regex,
                  site.getFunction())) {
    return false;
  }

  return true;
}

bool BreakPointInfo::checkClause() {
  TRACE(2, "BreakPointInfo::checkClause\n");
  if (!m_clause.empty()) {
    if (m_php.empty()) {
      if (m_check) {
        m_php = DebuggerProxy::MakePHPReturn(m_clause);
      } else {
        m_php = DebuggerProxy::MakePHP(m_clause);
      }
    }
    String output;
    {
      // Don't hit more breakpoints while attempting to decide if we should stop
      // at this breakpoint.
      EvalBreakControl eval(true);
      Variant ret = DebuggerProxy::ExecutePHP(m_php, output, false, 0);
      if (m_check) {
        return ret.toBoolean();
      }
    }
    m_output = std::string(output.data(), output.size());
    return true;
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////

void BreakPointInfo::SendImpl(int version, const BreakPointInfoPtrVec &bps,
                              DebuggerThriftBuffer &thrift) {
  TRACE(2, "BreakPointInfo::SendImpl\n");
  int16_t size = bps.size();
  thrift.write(size);
  for (int i = 0; i < size; i++) {
    bps[i]->sendImpl(version, thrift);
  }
}

void BreakPointInfo::RecvImpl(int version, BreakPointInfoPtrVec &bps,
                              DebuggerThriftBuffer &thrift) {
  TRACE(2, "BreakPointInfo::RecvImpl\n");
  int16_t size;
  thrift.read(size);
  bps.resize(size);
  for (int i = 0; i < size; i++) {
    BreakPointInfoPtr bpi(new BreakPointInfo());
    bpi->recvImpl(version, thrift);
    bps[i] = bpi;
  }
}

///////////////////////////////////////////////////////////////////////////////
}}
