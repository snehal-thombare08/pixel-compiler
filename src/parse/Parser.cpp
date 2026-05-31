#include "Parser.h"

#include "CompilerException.h"
#include "AST/Statement.h"
#include "ParserError.h"
#include "types/TypeContext.h"

Parser::Parser(std::vector<Token> tokens, TypeContext& typeCtx)
    : _tokens(std::move(tokens)), _position(0), _typeCtx(typeCtx) {
}

AST::Program Parser::parseProgram() {
    AST::Program program({0, 0, ""});

    while (!isAtEnd()) {
        try {
            program.addDeclaration(parseDeclaration());
        } catch (const ParseUnwindException& e) {
            synchronize();
        }
    }

    if (!_errors.empty()) {
        throw CompilerException(_errors);
    }

    return program;
}

std::unique_ptr<AST::Statement> Parser::parseDeclaration() {
    if (checkValue(Keyword::Var) || checkValue(Keyword::Const)) {
        return parseVariableDeclaration();
    }
    if (checkValue(Keyword::Func)) {
        return parseFunctionDeclaration();
    }

    logError(ParserErrorType::ExpectedDeclaration, peek());
}

std::unique_ptr<AST::Statement> Parser::parseStatement() {
    if (check<LeftBrace>()) {
        return parseBlock();
    }
    if (checkValue(Keyword::Var) || checkValue(Keyword::Const)) {
        return parseVariableDeclaration();
    }
    if (checkValue(Keyword::Return)) {
        return parseReturnStatement();
    }
    if (checkValue(Keyword::If)) {
        return parseIfStatement();
    }
    if (matchValue(Keyword::Else)) {
        logError(ParserErrorType::ElseWithoutIf, peekPrevious());
    }
    if (checkValue(Keyword::While)) {
        return parseWhileLoop();
    }
    if (checkValue(Keyword::For)) {
        return parseForLoop();
    }
    if (check<Identifier>()) {
        if (checkNext<LeftBracket>()) return parseArrayAssignment();
        if (checkNextValue(Operator::Assignment)) return parseVariableAssignment();
    }

    // Otherwise parse expression statement
    const auto startToken = peek();
    auto       expression = parseExpression();

    expect<Semicolon>();

    return std::make_unique<AST::ExpressionStatement>(startToken.metadata, std::move(expression));
}

std::unique_ptr<AST::Block> Parser::parseBlock() {
    const auto startToken = peek();
    expect<LeftBrace>();

    AST::Block block(peekPrevious().metadata);
    while (!check<RightBrace>()) {
        if (isAtEnd()) {
            logError(ParserErrorType::MissingClosingBrace, startToken);
        }
        try {
            block.statements.push_back(parseStatement());
        } catch (const ParseUnwindException& e) {
            if (checkValue(Keyword::Func)) {
                // if at start of a declaration, return to global scope
                throw ParseUnwindException();
            }
            synchronize();
        }
    }
    expect<RightBrace>();

    return std::make_unique<AST::Block>(std::move(block));
}

std::vector<AST::FunctionCall::FunctionArgument> Parser::parseFunctionArguments() {
    std::vector<AST::FunctionCall::FunctionArgument> args;
    while (!check<RightParen>()) {
        if (isAtEnd()) {
            logError(ParserErrorType::MissingClosingParen, peek());
        }
        std::optional<std::string> paramName;

        if (check<Identifier>() && checkNext<Colon>()) {
            // parameter with identifier: value
            paramName = expect<Identifier>().name;
            expect<Colon>();
        }
        auto param = parseExpression();

        args.emplace_back(std::move(param), paramName);
        if (!check<RightParen>()) {
            expect<Comma>(); // if didn't read the end, get a comma seperator
            // Check for trailing comma
            if (check<RightParen>())
                logError(ParserErrorType::TrailingComma, peek());
        }
    }

    return args;
}

std::unique_ptr<AST::Statement> Parser::parseFunctionDeclaration() {
    expect<Keyword>();
    auto [name]       = expect<Identifier>();
    auto namePosition = peekPrevious().metadata;
    expect<LeftParen>();

    std::vector<AST::FunctionDeclaration::FunctionParameter> parameters;
    //parse parameters
    while (!check<RightParen>()) {
        if (isAtEnd()) {
            logError(ParserErrorType::MissingClosingParen, peek());
        }
        bool isImplicit = match<Underscore>();

        auto paramName = expect<Identifier>();
        expect<Colon>();
        auto paramType = expect<PrimitiveKind>();

        parameters.emplace_back(paramName.name, _typeCtx.get(paramType), isImplicit);
        if (!check<RightParen>()) {
            expect<Comma>(); // if didn't read the end, get a comma seperator
            // Check for trailing comma
            if (check<RightParen>())
                logError(ParserErrorType::TrailingComma, peek());
        }
    }

    expect<RightParen>();
    expect<Arrow>();

    auto returnType = expect<PrimitiveKind>();
    auto block      = parseBlock();

    return std::make_unique<AST::FunctionDeclaration>(namePosition, _typeCtx.get(returnType), name, parameters,
                                                      std::move(block));
}

std::unique_ptr<AST::Statement> Parser::parseVariableDeclaration() {
    const bool isConst = checkValue(Keyword::Const);
    expect<Keyword>();

    const auto varNameToken = peek();
    auto       [name]       = expect<Identifier>();

    // var name[N] — explicit fixed-size array
    int arraySize = 0;
    if (match<LeftBracket>()) {
        if (checkValue(Operator::Minus))
            logError(ParserErrorType::NonPositiveArraySize, peek());
        const auto [size] = expect<IntegerLiteral>();
        if (size < 1)
            logError(ParserErrorType::NonPositiveArraySize, peekPrevious());
        arraySize = size;
        expect<RightBracket>();
    }

    // : type
    PrimitiveKind elementKind = PrimitiveKind::Unspecified;
    if (match<Colon>())
        elementKind = expect<PrimitiveKind>();

    // = value
    std::unique_ptr<AST::Expression> value;
    if (matchValue(Operator::Assignment)) {
        if (check<LeftBracket>()) {
            value = parseArrayLiteral();
            if (arraySize == 0) arraySize = -1; // size inferred by type checker
        } else {
            value = parseExpression();
        }
    }

    if (!value && elementKind == PrimitiveKind::Unspecified)
        logError(ParserErrorType::TypelessVarDeclaration, varNameToken);

    expect<Semicolon>();

    TypeNode* elementType  = _typeCtx.get(elementKind); // nullptr if Unspecified
    TypeNode* declaredType = (arraySize != 0)
                                 ? _typeCtx.getArray(elementType, arraySize)
                                 : elementType;

    return std::make_unique<AST::VariableDeclaration>(
        varNameToken.metadata, isConst, declaredType, name, std::move(value)
    );
}

std::unique_ptr<AST::Statement> Parser::parseVariableAssignment() {
    auto       name    = expect<Identifier>();
    const auto namePos = peekPrevious().metadata;

    expectValue(Operator::Assignment);

    auto value = parseExpression();
    expect<Semicolon>();

    return std::make_unique<AST::VariableAssignment>(namePos, name.name, std::move(value));
}

std::unique_ptr<AST::Statement> Parser::parseArrayAssignment() {
    auto       name    = expect<Identifier>();
    const auto namePos = peekPrevious().metadata;

    expect<LeftBracket>();
    auto index = parseExpression();
    expect<RightBracket>();

    expectValue(Operator::Assignment);
    auto value = parseExpression();
    expect<Semicolon>();

    return std::make_unique<AST::ArrayAssignment>(namePos, name.name, std::move(index), std::move(value));
}

std::unique_ptr<AST::Expression> Parser::parseFunctionCall() {
    auto       [name]  = expect<Identifier>();
    const auto namePos = peekPrevious().metadata;

    expect<LeftParen>();
    auto arguments = parseFunctionArguments();
    expect<RightParen>();

    return std::make_unique<AST::FunctionCall>(namePos, name, std::move(arguments));
}

std::unique_ptr<AST::Statement> Parser::parseIfStatement() {
    expect<Keyword>();
    const auto ifPos     = peekPrevious().metadata;
    auto       condition = parseExpression();

    auto thenBranch = parseStatement();

    std::unique_ptr<AST::Statement> elseBranch = nullptr;

    if (matchValue(Keyword::Else)) {
        if (checkValue(Keyword::If)) {
            elseBranch = parseIfStatement(); // nested if
        } else {
            elseBranch = parseStatement(); // normal else
        }
    }
    return std::make_unique<AST::IfStatement>(ifPos, std::move(condition), std::move(thenBranch),
                                              std::move(elseBranch));
}

std::unique_ptr<AST::Statement> Parser::parseWhileLoop() {
    expect<Keyword>();
    const auto whilePos  = peekPrevious().metadata;
    auto       condition = parseExpression();

    auto block = parseStatement();

    return std::make_unique<AST::WhileLoop>(whilePos, std::move(condition), std::move(block));
}

std::unique_ptr<AST::RangeExpression> Parser::parseRangeExpression() {
    const auto startPosition = peek().metadata;
    auto       start         = parseExpression();
    expect<DoubleDot>();
    auto end = parseExpression();

    return std::make_unique<AST::RangeExpression>(startPosition, std::move(start), std::move(end));
}

std::unique_ptr<AST::Statement> Parser::parseForLoop() {
    expect<Keyword>();
    const auto forPos     = peekPrevious().metadata;
    auto       identifier = expect<Identifier>();
    expectValue(Keyword::In);
    auto range = parseRangeExpression();

    std::unique_ptr<AST::Expression> step = std::make_unique<AST::IntegerLiteralNode>(peek().metadata, 1);
    if (matchValue(Keyword::Step)) {
        step = parseExpression();
    }

    auto body = parseStatement();

    return std::make_unique<AST::ForLoop>(forPos, identifier.name, std::move(range), std::move(step), std::move(body));
}

std::unique_ptr<AST::Statement> Parser::parseReturnStatement() {
    expect<Keyword>();
    const auto returnPos = peekPrevious().metadata;

    std::unique_ptr<AST::Expression> value;

    if (!check<Semicolon>())
        value = parseExpression();
    expect<Semicolon>();

    return std::make_unique<AST::ReturnStatement>(returnPos, std::move(value));
}

std::unique_ptr<AST::Expression> Parser::parseExpression() {
    return parseBooleanOrExpression();
}


std::unique_ptr<AST::Expression> Parser::parseBooleanOrExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseBooleanAndExpression();

    while (checkValue(Operator::LogicalOr)) {
        auto op    = expect<Operator>();
        auto right = parseBooleanAndExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<AST::Expression> Parser::parseBooleanAndExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseBooleanEqualityExpression();

    while (checkValue(Operator::LogicalAnd)) {
        auto op    = expect<Operator>();
        auto right = parseBooleanEqualityExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}


std::unique_ptr<AST::Expression> Parser::parseBooleanEqualityExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseComparisonExpression();

    while (checkValue(Operator::Equal) || checkValue(Operator::NotEqual)) {
        auto op    = expect<Operator>();
        auto right = parseComparisonExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<AST::Expression> Parser::parseComparisonExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseAdditiveExpression();

    while (checkValue(Operator::LessThan) || checkValue(Operator::LessEqual)
           || checkValue(Operator::GreaterThan) || checkValue(Operator::GreaterEqual)) {
        auto op    = expect<Operator>();
        auto right = parseAdditiveExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<AST::Expression> Parser::parseAdditiveExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseMultiplicativeExpression();

    while (checkValue(Operator::Plus) || checkValue(Operator::Minus)) {
        auto op    = expect<Operator>();
        auto right = parseMultiplicativeExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<AST::Expression> Parser::parseMultiplicativeExpression() {
    const auto startPosition = peek().metadata;
    auto       left          = parseUnaryExpression();

    while (checkValue(Operator::Star) || checkValue(Operator::Slash)) {
        auto op    = expect<Operator>();
        auto right = parseUnaryExpression();

        left = std::make_unique<AST::BinaryExpression>(startPosition, std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<AST::Expression> Parser::parseUnaryExpression() {
    if (checkValue(Operator::Plus) || checkValue(Operator::Minus) ||
        checkValue(Operator::Exclamation)) {
        auto op      = expect<Operator>();
        auto operand = parseUnaryExpression(); // to allow -++i

        return std::make_unique<AST::UnaryExpression>(peekPrevious().metadata, std::move(operand), op);
    }
    return parseIncDecExpression();
}

std::unique_ptr<AST::Expression> Parser::parseIncDecExpression() {
    // check for prefix ++ and --
    if (checkValue(Operator::PlusPlus) || checkValue(Operator::MinusMinus)) {
        if (!checkNext<Identifier>()) {
            logError(ParserErrorType::IncrementNonVariable, peek());
        }

        auto op     = expect<Operator>();
        auto var    = expect<Identifier>();
        auto fixPos = AST::IncDecExpression::Fix::Prefix;

        return std::make_unique<AST::IncDecExpression>(peekPrevious().metadata, var.name, op, fixPos);
    }
    // check for postfix ++ and --
    auto expr = parsePrimary();

    while (checkValue(Operator::PlusPlus) || checkValue(Operator::MinusMinus)) {
        if (auto var = dynamic_cast<AST::VariableExpression*>(expr.get())) {
            auto op     = expect<Operator>();
            auto fixPos = AST::IncDecExpression::Fix::Postfix;
            expr        = std::make_unique<AST::IncDecExpression>(peekPrevious().metadata, var->name, op, fixPos);
        } else {
            logError(ParserErrorType::IncrementNonVariable, peek());
        }
    }
    return expr;
}

std::unique_ptr<AST::Expression> Parser::parsePrimary() {
    if (check<IntegerLiteral>()) {
        return std::make_unique<AST::IntegerLiteralNode>(peek().metadata, expect<IntegerLiteral>().value);
    }
    if (check<FloatLiteral>()) {
        return std::make_unique<AST::FloatLiteralNode>(peek().metadata, expect<FloatLiteral>().value);
    }
    if (check<BooleanLiteral>()) {
        return std::make_unique<AST::BooleanLiteralNode>(peek().metadata, expect<BooleanLiteral>().value);
    }
    if (check<StringLiteral>()) {
        return std::make_unique<AST::StringLiteralNode>(peek().metadata, expect<StringLiteral>().value);
    }
    if (check<Identifier>()) {
        if (checkNext<LeftParen>()) return parseFunctionCall();
        if (checkNext<LeftBracket>()) return parseArrayIndex();

       const auto pos = peek().metadata;
auto variable = std::make_unique<AST::VariableExpression>(pos, expect<Identifier>().name);

        return variable;
    }
    if (check<LeftParen>()) {
        expect<LeftParen>();
        auto expression = parseExpression();
        expect<RightParen>();
        return expression;
    }
    logError(ParserErrorType::ExpectedExpression, peek());
}

std::unique_ptr<AST::Expression> Parser::parseArrayLiteral() {
    expect<LeftBracket>();

    std::vector<std::unique_ptr<AST::Expression> > elements;
    if (!check<RightBracket>()) {
        elements.push_back(parseExpression());
        while (check<Comma>()) {
            expect<Comma>();
            elements.push_back(parseExpression());
        }
    }
    expect<RightBracket>();

    return std::make_unique<AST::ArrayLiteral>(peekPrevious().metadata, std::move(elements));
}

std::unique_ptr<AST::Expression> Parser::parseArrayIndex() {
    const auto [arrayName] = expect<Identifier>();
    expect<LeftBracket>();
    auto index = parseExpression();
    expect<RightBracket>();

    return std::make_unique<AST::ArrayIndex>(peekPrevious().metadata, arrayName, std::move(index));
}


Token& Parser::peek() {
    if (_position >= _tokens.size()) {
        return _endOfFileToken;
    }
    return _tokens[_position];
}

Token& Parser::peekPrevious() {
    if (_position == 0)
        return _endOfFileToken;
    return _tokens[_position - 1];
}

Token& Parser::peekNext() {
    auto position = _position + 1;
    if (position >= _tokens.size()) {
        return _endOfFileToken;
    }
    return _tokens[position];
}

template<typename T>
bool Parser::check() {
    return std::holds_alternative<T>(peek().type);
}

template<typename T>
bool Parser::checkNext() {
    return std::holds_alternative<T>(peekNext().type);
}

template<typename T>
bool Parser::match() {
    const bool match = check<T>();
    if (match) advance();
    return match;
}

template<typename T>
bool Parser::matchValue(T value) {
    const bool match = checkValue(value);
    if (match) advance();
    return match;
}

template<typename T>
bool Parser::checkValue(T value) {
    return check<T>() && std::get<T>(peek().type) == value;
}

template<typename T>
bool Parser::checkNextValue(T value) {
    return checkNext<T>() && std::get<T>(peekNext().type) == value;
}

void Parser::advance() {
    if (!isAtEnd()) _position++;
}

template<typename T>
T Parser::expect() {
    if (check<T>()) {
        T value = std::get<T>(peek().type);
        advance();
        return value;
    }

    // error() throws ParseUnwindException, so this line
    // technically never returns, but we call it to log and throw.
    logError(ParserErrorType::UnexpectedToken, peek(), T());
}

template<typename T>
T Parser::expectValue(T value) {
    if (!checkValue(value)) {
        logError(ParserErrorType::UnexpectedToken, peek(), value);
    }
    T returnValue = std::get<T>(peek().type);
    advance();
    return returnValue;
}

bool Parser::isAtEnd() {
    return check<EndOfFile>();
}

void Parser::synchronize() {
    _isPanicMode = false;

    // IMPORTANT: If we are synchronizing, the current token is
    // part of the failure. We MUST consume it to avoid infinite loops,
    // even if it looks like the start of a statement.
    if (!isAtEnd()) advance();

    while (!isAtEnd()) {
        if (std::holds_alternative<Semicolon>(peekPrevious().type)) return;
        if (isAtStartOfStatement()) return;
        advance();
    }
}

bool Parser::isAtStartOfStatement() {
    if (check<Keyword>()) {
        switch (std::get<Keyword>(peek().type)) {
            case Keyword::Func:
            case Keyword::Var:
            case Keyword::Const:
            case Keyword::If:
            case Keyword::While:
            case Keyword::Return:
            case Keyword::For:
                return true;
            default:
                return false;
        }
    }
    if (check<LeftBrace>()) return true;
    return false;
}

void Parser::logError(const ParserErrorType& type, const Token& errorToken, const TokenType& expectedTokenType) {
    _isPanicMode = true;

    _errors.push_back(ParserError(type, errorToken, expectedTokenType));

    // Unwind the stack to the nearest synchronization point
    throw ParseUnwindException();
}
