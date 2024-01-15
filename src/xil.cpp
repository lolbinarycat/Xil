#include "xil.hpp"
#include "attriter.hpp"

#include <iterator>

#include <cppitertools/itertools.hpp>
#include <fmt/args.h>
#include <fmt/format.h>

// Nix headers.
#include <print.hh>

using namespace std::literals::string_literals;

std::string_view nix::format_as(nix::ValueType const type) noexcept
{
	switch (type) {
		case nix::nThunk:
			return "thunk";
		case nix::nInt:
			return "integer";
		case nix::nFloat:
			return "float";
		case nix::nBool:
			return "bool";
		case nix::nString:
			return "string";
		case nix::nPath:
			return "path";
		case nix::nNull:
			return "null";
		case nix::nAttrs:
			return "attrs";
		case nix::nList:
			return "list";
		case nix::nFunction:
			return "function";
		case nix::nExternal:
			return "external?";
		default:
			return "«unreachable»";
	}
}

std::optional<nix::SymbolStr> Printer::symbol(nix::Symbol &&symbol)
{
	if (!symbol) {
		return std::nullopt;
	}

	return this->state->symbols[symbol];
}

OptString Printer::symbolStr(nix::Symbol &symbol)
{
	if (!symbol) {
		return std::nullopt;
	}

	return static_cast<std::string>(this->state->symbols[symbol]);
}

OptStringView Printer::symbolStrView(nix::Symbol &symbol)
{
	if (!symbol) {
		return std::nullopt;
	}

	return static_cast<std::string_view>(this->state->symbols[symbol]);
}

nix::Value *Printer::getAttrValue(nix::Bindings *attrs, nix::Symbol key)
{
	assert(attrs != nullptr);

	auto *found = attrs->find(key);

	assert(found != nullptr);

	return found->value;
}

nix::Value *Printer::getAttrValue(nix::Bindings *attrs, std::string_view key)
{
	// FIXME: does this...work?
	for (auto &&attr : *attrs) {
		auto attrName = this->symbolStrView(attr.name);
		if (attrName == key) {
			return attr.value;
		}
	}

	return nullptr;
}

OptString Printer::exprName(nix::Expr *expr)
{
	assert(expr != nullptr);

	// There aren't any methods we can use to get what kind of expression
	// expr is, so we use RTTI to convert it to an std::variant, and then
	// match on it.
	auto polymorphicExpr = ExprT::from(expr);

	return std::visit(overloaded {
		[&](nix::ExprVar *expr) -> OptString {
			// FIXME: Can a variable have an invalid Symbol as its name?
			assert(expr->name);

			// Variables have a name. Use it!
			return this->symbolStr(expr->name);
		},
		[&](nix::ExprLambda *expr) -> OptString {
			// If this lambda has a name, then use that.
			if (expr->name) {
				return this->symbolStr(expr->name);
			}
			return std::nullopt;
		},
		[&](nix::ExprCall *) -> OptString {
			// TODO: should this use the name of the function its calling?
			return std::nullopt;
		},
		[&](auto *) -> OptString {
			return std::nullopt;
		},
	}, polymorphicExpr);
}

OptString Printer::valueName(nix::Value &value)
{
	// Unlike `Expr`s, we can just query the value's type normally.

	switch (value.type()) {
		case nix::nAttrs:
			// TODO: can we do anything here?
			return std::nullopt;
		case nix::nThunk:
			// TODO: optionally try-force?
			return std::nullopt;

		case nix::nFunction:
			// Lambdas, primops, and applications of primops are all considered "functions",
			// but Value::type().
			// Their union members are different, though, so we need to check each case.
			if (value.isLambda()) {
				assert(value.lambda.fun != nullptr);
				return this->exprName(value.lambda.fun);
			} else if (value.isPrimOp()) {
				assert(value.primOp != nullptr);
				return value.primOp->name;
			} else if (value.isPrimOpApp()) {
				assert(value.primOpApp.left != nullptr);
				assert(value.primOpApp.right != nullptr);

				// FIXME: is this the right thing to do?
				auto leftName = this->valueName(*value.primOpApp.left);
				auto rightName = this->valueName(*value.primOpApp.right);
				auto spacer = (leftName.has_value() && rightName.has_value()) ? " " : "";
				return fmt::format("{}{}{}", leftName.value_or(""), spacer, rightName.value_or(""));
			} else {
				assert("unreachable" == nullptr);
			}
		default:
			return std::nullopt;
	}
}

static void printIndent(std::ostream &out, uint32_t indentLevel)
{
	// TODO: allow configuring the indentation size.
	for ([[maybe_unused]] uint32_t i : iter::range(indentLevel * 4)) {
		out << " ";
	}
}


std::ostream &operator<<(std::ostream &out, Indent const &self)
{
	printIndent(out, self.level);
	return out;
}

// A formatter for fmt::format().
std::string format_as(Indent const indentation)
{
	std::stringstream ss;
	printIndent(ss, indentation.level);
	return ss.str();
}

// Prints single-line strings in quotes, and multiline strings as a '' string, formatted nicely.
std::string prettyString(std::string_view nixString, uint32_t indentLevel)
{
	//fmt::dynamic_format_arg_store<fmt::format_context> result;
	// TODO: this, or std::stringstream?
	auto buffer = fmt::memory_buffer();

	bool multiline = nixString.find("\n") != decltype(nixString)::npos;
	if (multiline) {
		fmt::format_to(std::back_inserter(buffer), "''\n");
	} else {
		fmt::format_to(std::back_inserter(buffer), "\"");
	}

	bool needsIndent = multiline;

	for (auto &&ch : nixString) {
		if (needsIndent) {
			// + 1 because things indented in the string need to be indented *further* than the string.
			fmt::format_to(std::back_inserter(buffer), "{}", Indent(indentLevel + 1));
			needsIndent = false;
		}

		switch (ch) {
			case '\\':
				fmt::format_to(std::back_inserter(buffer), "\\");
				break;
			case '\n':
				if (multiline) {
					fmt::format_to(std::back_inserter(buffer), "\n");
					needsIndent = true;
				} else {
					fmt::format_to(std::back_inserter(buffer), "\\n");
				}
				break;
			case '\r':
				fmt::format_to(std::back_inserter(buffer), "\\r");
				break;
			case '$': {
				// Minor hack: check if the next character is an open brace, which we will need to escape.
				auto *next = (&ch + 1);
				if (next != nixString.end() && *next == '{') {
					// The escape mechanism depends on whether or not it's a '' string.
					if (multiline) {
						fmt::format_to(std::back_inserter(buffer), "''$");
					} else {
						fmt::format_to(std::back_inserter(buffer), "$$");
					}
				} else {
					fmt::format_to(std::back_inserter(buffer), "$");
				}
				break;
			}
			// FIXME: allow custom size for \t
			default:
				fmt::format_to(std::back_inserter(buffer), "{:c}", ch);
				break;
		}
	}

	if (needsIndent) {
		// No more + 1 for the last part, since this is the closing part of the string.
		fmt::format_to(std::back_inserter(buffer), "{}", Indent(indentLevel));
	}

	if (multiline) {
		fmt::format_to(std::back_inserter(buffer), "''");
	} else {
		fmt::format_to(std::back_inserter(buffer), "\"");
	}

	return fmt::to_string(buffer);
}

void Printer::printValue(nix::Value &value, std::ostream &out, uint32_t indentLevel)
{
	// FIXME
	assert(indentLevel < 100);

	// If we have a thunk, *try* to force it before we print it.
	// If there's an error, catch it and print a short version of the error.
	// FIXME: make configurable.
	if (value.type() == nix::nThunk) {
		try {
			this->state->forceValue(value, nix::noPos);
		} catch (nix::ThrownError &ex) {
			// FIXME: parse out error
			out << "«throws»";
			return;
		}
	}

	switch (value.type()) {
		case nix::nThunk:
			out << "«thunk»";
			break;
		case nix::nInt:
			out << value.integer;
			break;
		case nix::nFloat:
			out << value.fpoint;
			break;
		case nix::nBool:
			nix::printLiteralBool(out, value.boolean);
			break;
		case nix::nString:
			out << prettyString(value.str(), indentLevel);
			break;
		case nix::nPath:
			out << value.path().to_string();
			break;
		case nix::nNull:
			out << "null";
			break;
		case nix::nAttrs: {
			if (this->state->isDerivation(value)) {
				auto drvPath = this->getAttrValue(value.attrs, this->state->sDrvPath);
				assert(drvPath != nullptr);
				out << "«derivation ";
				// We handle drvPath specially because anything other than a string
				// should be invalid, and if it is a string then we don't want to print
				// the quotes (which this->printValue() adds).
				if (drvPath->isThunk()) {
					try {
						this->state->forceValue(*drvPath, nix::noPos);
					} catch (nix::ThrownError &ex) {
						out << "«throws»»";
						return;
					}
				}

				if (drvPath->type() != nix::nString) {
					out << fmt::format("invalid {}", drvPath->type());
				} else {
					out << drvPath->string.s;
				}
				out << "»";

				break;
			}

			auto attrIter = AttrIterable(value.attrs, this->state->symbols);

			// FIXME: hardcodes pkgs recursion.
			auto typeIsPkgs = std::find_if(
				attrIter.begin(),
				attrIter.end(),
				[](std::tuple<std::string_view const, nix::Value const &> pair) -> bool {
					auto const &[name, value] = pair;
					return name == "_type" &&
						value.type() == nix::nString &&
						std::string_view{value.string.s} == "pkgs"s;
				}
			);
			bool isPkgs = typeIsPkgs != attrIter.end();

			if (isPkgs && indentLevel > 0) {
				out << "«too deep»";
				return;
			}

			// FIXME: better heuristics for short attrsets.
			if (attrIter.empty()) {
				out << "{ }";
				return;
			}

			out << "{";

			for (auto const &[name, value] : attrIter) {
				out << "\n" << Indent(indentLevel + 1) << name << " = ";
				std::flush(out);
				this->printValue(value, out, indentLevel + 1);
				out << ";";
			}

			out << "\n" << Indent(indentLevel) << "}";

			break;
		}
		case nix::nList: {
			// FIXME: better heuristics for short lists
			// Things like `outputs = [ "out" ]` are annoying printed multiline.
			if (value.listSize() == 0) {
				out << "[ ]";
				break;
			}

			out << "[";
			for (auto &listItem : value.listItems()) {
				out << "\n" << Indent(indentLevel + 1);
				std::flush(out);
				this->printValue(*listItem, out, indentLevel + 1);
			}

			out << "\n" << Indent(indentLevel) << "]";

			break;
		}
		case nix::nFunction:
			out << "«function»";
			break;
		case nix::nExternal:
			out << "«external?»";
			break;
	}
}
