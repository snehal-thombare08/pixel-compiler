#include "Compiler.h"

#include <format>

#include "semantic/SymbolPool.h"
#include "semantic/SymbolTable.h"
#include <fstream>
#include <sstream>
#include <utility>

#include "IR/IRGeneratorLLVM.h"
#include "parse/Parser.h"
#include "semantic/DeclarationPassVisitor.h"
#include "semantic/TypeCheckerVisitor.h"

Compiler::Compiler(std::string sourceFile)
    : _sourceFileName(std::move(sourceFile)) {
    std::ifstream file(_sourceFileName);
    if (!file.is_open()) {
        throw std::invalid_argument(std::format("Failed to open file: '{}'", sourceFile));
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    _sourceCode = buffer.str();

    initFunctions();
    initGlobals();
}

void Compiler::compile() {
    // Lexing
    Lexer lexer(_sourceCode);

    const auto tokens = lexer.lex();

    // Parsing
    Parser parser(tokens, _typeContext);
    auto   ast = parser.parseProgram();

    // Semantic analyzing
    SymbolPool  symbols;
    SymbolTable symbolTable(symbols);
    symbolTable.declareBuiltinFunctions(_functionRegistry.getAllApiFunctions());
    symbolTable.declareBuiltinGlobals(_globalRegistry.getAllGlobals());

    DeclarationPassVisitor declPass(symbolTable, _typeContext);
    declPass.run(ast);

    TypeCheckerVisitor typeChecker(symbolTable, _typeContext);
    typeChecker.run(ast);

    IRGeneratorLLVM irGenerator(_typeContext, _functionRegistry, _globalRegistry);
    irGenerator.visit(ast);

   irGenerator.createExecutable("out");
}

void Compiler::initFunctions() {
    const auto intType     = _typeContext.getInt();
    const auto floatType   = _typeContext.getFloat();
    const auto stringType  = _typeContext.getString();
    const auto voidType    = _typeContext.getVoid();
    const auto boolType    = _typeContext.getBool();
    const auto pointerType = _typeContext.getPointer();

    // -----------------------------
    // Math & IO API — user-callable
    // -----------------------------
    _functionRegistry.registerApi("print",
                                  {{{"str", stringType, true}}, intType, "printf", true});

    // Math — LLVM intrinsics (float in, float out)
    _functionRegistry.registerIntrinsic("abs", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::fabs);
    _functionRegistry.registerIntrinsic("sqrt", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::sqrt);
    _functionRegistry.registerIntrinsic(
        "pow", {{{"base", floatType}, {"exponent", floatType}}, floatType},
        llvm::Intrinsic::pow);
    _functionRegistry.registerIntrinsic("sin", {{{"angle", floatType, true}}, floatType},
                                        llvm::Intrinsic::sin);
    _functionRegistry.registerIntrinsic("cos", {{{"angle", floatType, true}}, floatType},
                                        llvm::Intrinsic::cos);
    _functionRegistry.registerIntrinsic("exp", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::exp);
    _functionRegistry.registerIntrinsic("exp2", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::exp2);
    _functionRegistry.registerIntrinsic("log", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::log);
    _functionRegistry.registerIntrinsic("log2", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::log2);
    _functionRegistry.registerIntrinsic("log10", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::log10);
    _functionRegistry.registerIntrinsic("floor", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::floor);
    _functionRegistry.registerIntrinsic("ceil", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::ceil);
    _functionRegistry.registerIntrinsic("round", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::round);
    _functionRegistry.registerIntrinsic("trunc", {{{"value", floatType, true}}, floatType},
                                        llvm::Intrinsic::trunc);
    _functionRegistry.registerIntrinsic(
        "min", {{{"a", floatType}, {"b", floatType}}, floatType},
        llvm::Intrinsic::minnum);
    _functionRegistry.registerIntrinsic(
        "max", {{{"a", floatType}, {"b", floatType}}, floatType},
        llvm::Intrinsic::maxnum);
    _functionRegistry.registerIntrinsic(
        "copysign", {{{"mag", floatType}, {"sgn", floatType}}, floatType},
        llvm::Intrinsic::copysign);
    _functionRegistry.registerApi("tan",
                                  {
                                      {{"angle", floatType, true}},
                                      floatType, "tanf"
                                  });
    _functionRegistry.registerApi("asin",
                                  {
                                      {{"value", floatType, true}},
                                      floatType, "asinf"
                                  });
    _functionRegistry.registerApi("acos",
                                  {
                                      {{"value", floatType, true}},
                                      floatType, "acosf"
                                  });
    _functionRegistry.registerApi("atan",
                                  {
                                      {{"value", floatType, true}},
                                      floatType, "atanf"
                                  });
    _functionRegistry.registerApi("atan2",
                                  {
                                      {{"y", floatType}, {"x", floatType}},
                                      floatType, "atan2f"
                                  });

    // -----------------------------
    // Graphics — Structure
    // -----------------------------
    _functionRegistry.registerApi("loop",
                                  {{}, voidType, "pxl_loop"});
    _functionRegistry.registerApi("noLoop",
                                  {{}, voidType, "pxl_no_loop"});

    // -----------------------------
    // Graphics — Canvas setup
    // -----------------------------
    _functionRegistry.registerApi("canvas",
                                  {
                                      {
                                          {"width", intType},
                                          {"height", intType}
                                      },
                                      voidType, "pxl_set_canvas_size"
                                  });
    _functionRegistry.registerApi("frameRate",
                                  {
                                      {{"fps", intType}}, voidType,
                                      "pxl_set_frames_per_second"
                                  });
    _functionRegistry.registerApi("title",
                                  {
                                      {{"t", stringType}}, voidType,
                                      "pxl_set_window_title"
                                  });

    // -----------------------------
    // Graphics — Color / settings
    // -----------------------------
    _functionRegistry.registerApi("background",
                                  {
                                      {
                                          {"r", floatType},
                                          {"g", floatType}, {"b", floatType}
                                      },
                                      voidType,
                                      "pxl_background"
                                  });
    _functionRegistry.registerApi("fill",
                                  {
                                      {
                                          {"r", floatType},
                                          {"g", floatType}, {"b", floatType}
                                      },
                                      voidType,
                                      "pxl_fill"
                                  });
    _functionRegistry.registerApi("fillAlpha",
                                  {
                                      {
                                          {"r", floatType},
                                          {"g", floatType},
                                          {"b", floatType}, {"a", floatType}
                                      },
                                      voidType, "pxl_fill_a"
                                  });
    _functionRegistry.registerApi("noFill",
                                  {{}, voidType, "pxl_no_fill"});
    _functionRegistry.registerApi("stroke",
                                  {
                                      {
                                          {"r", floatType},
                                          {"g", floatType}, {"b", floatType}
                                      },
                                      voidType,
                                      "pxl_stroke"
                                  });
    _functionRegistry.registerApi("strokeAlpha",
                                  {
                                      {
                                          {"r", floatType},
                                          {"g", floatType},
                                          {"b", floatType}, {"a", floatType}
                                      },
                                      voidType, "pxl_stroke_a"
                                  });
    _functionRegistry.registerApi("noStroke",
                                  {{}, voidType, "pxl_no_stroke"});
    _functionRegistry.registerApi("strokeWeight",
                                  {
                                      {{"weight", floatType}}, voidType,
                                      "pxl_stroke_weight"
                                  });
    _functionRegistry.registerApi("colorMode",
                                  {
                                      {{"mode", intType}}, voidType,
                                      "pxl_color_mode"
                                  });

    // -----------------------------
    // Graphics — 2D Primitives
    // -----------------------------
    _functionRegistry.registerApi("rect",
                                  {
                                      {
                                          {"x", floatType},
                                          {"y", floatType},
                                          {"w", floatType}, {"h", floatType}
                                      },
                                      voidType, "pxl_rect"
                                  });
    _functionRegistry.registerApi("circle",
                                  {
                                      {
                                          {"x", floatType},
                                          {"y", floatType}, {"d", floatType}
                                      },
                                      voidType,
                                      "pxl_circle"
                                  });
    _functionRegistry.registerApi("ellipse",
                                  {
                                      {
                                          {"x", floatType},
                                          {"y", floatType},
                                          {"w", floatType}, {"h", floatType}
                                      },
                                      voidType, "pxl_ellipse"
                                  });
    _functionRegistry.registerApi("line",
                                  {
                                      {
                                          {"x1", floatType},
                                          {"y1", floatType},
                                          {"x2", floatType},
                                          {"y2", floatType}
                                      },
                                      voidType, "pxl_line"
                                  });
    _functionRegistry.registerApi("triangle",
                                  {
                                      {
                                          {"x1", floatType},
                                          {"y1", floatType},
                                          {"x2", floatType},
                                          {"y2", floatType},
                                          {"x3", floatType},
                                          {"y3", floatType}
                                      },
                                      voidType, "pxl_triangle"
                                  });
    _functionRegistry.registerApi("point",
                                  {
                                      {{"x", floatType}, {"y", floatType}},
                                      voidType, "pxl_point"
                                  });

    // -----------------------------
    // Graphics — Transforms
    // -----------------------------
    _functionRegistry.registerApi("translate",
                                  {
                                      {{"x", floatType}, {"y", floatType}},
                                      voidType, "pxl_translate"
                                  });
    _functionRegistry.registerApi("rotate",
                                  {
                                      {{"angle", floatType}}, voidType,
                                      "pxl_rotate"
                                  });
    _functionRegistry.registerApi("scale",
                                  {
                                      {
                                          {"sx", floatType},
                                          {"sy", floatType}
                                      },
                                      voidType, "pxl_scale"
                                  });
    _functionRegistry.registerApi("push",
                                  {{}, voidType, "pxl_push"});
    _functionRegistry.registerApi("pop",
                                  {{}, voidType, "pxl_pop"});

    // -----------------------------
    // Graphics — Math utilities
    // -----------------------------
    _functionRegistry.registerApi("map",
                                  {
                                      {
                                          {"value", floatType},
                                          {"start1", floatType},
                                          {"stop1", floatType},
                                          {"start2", floatType},
                                          {"stop2", floatType}
                                      },
                                      floatType, "pxl_map"
                                  });
    _functionRegistry.registerApi("lerp",
                                  {
                                      {
                                          {"start", floatType},
                                          {"stop", floatType},
                                          {"amt", floatType}
                                      },
                                      floatType, "pxl_lerp"
                                  });
    _functionRegistry.registerApi("constrain",
                                  {
                                      {
                                          {"n", floatType},
                                          {"low", floatType},
                                          {"high", floatType}
                                      },
                                      floatType,
                                      "pxl_constrain"
                                  });
    _functionRegistry.registerApi("dist",
                                  {
                                      {
                                          {"x1", floatType},
                                          {"y1", floatType},
                                          {"x2", floatType},
                                          {"y2", floatType}
                                      },
                                      floatType, "pxl_dist"
                                  });
    _functionRegistry.registerApi("random",
                                  {
                                      {
                                          {"low", floatType},
                                          {"high", floatType}
                                      },
                                      floatType, "pxl_random"
                                  });
    _functionRegistry.registerApi("noise",
                                  {
                                      {{"x", floatType}}, floatType,
                                      "pxl_noise"
                                  });
    _functionRegistry.registerApi("noise2",
                                  {
                                      {{"x", floatType}, {"y", floatType}},
                                      floatType, "pxl_noise2"
                                  });

    // -----------------------------
    // Graphics — Typography
    // -----------------------------
    _functionRegistry.registerApi("text",
                                  {
                                      {
                                          {"str", stringType},
                                          {"x", floatType}, {"y", floatType}
                                      },
                                      voidType,
                                      "pxl_text"
                                  });
    _functionRegistry.registerApi("textSize",
                                  {
                                      {{"size", floatType}}, voidType,
                                      "pxl_text_size"
                                  });
    _functionRegistry.registerApi("textAlign",
                                  {
                                      {{"align", intType}}, voidType,
                                      "pxl_text_align"
                                  });

    // -----------------------------
    // Graphics — Lifecycle (internal)
    // -----------------------------
    _functionRegistry.registerInternal("run",
                                       {
                                           {
                                               {"setup", pointerType},
                                               {"draw", pointerType}
                                           },
                                           voidType, "pxl_run"
                                       });
    _functionRegistry.registerInternal("quit",
                                       {{}, voidType, "pxl_quit"});

    // -----------------------------
    // Runtime string helpers — internal
    // -----------------------------
    _functionRegistry.registerInternal("pxl_create_string",
                                       {
                                           {
                                               {"data", stringType},
                                               {"size", intType}
                                           },
                                           stringType
                                       });
    _functionRegistry.registerInternal("pxl_destroy_string",
                                       {{{"str", stringType}}, voidType});
    _functionRegistry.registerInternal("pxl_copy",
                                       {
                                           {
                                               {"dest", stringType},
                                               {"src", stringType}
                                           },
                                           voidType
                                       });
    _functionRegistry.registerInternal("pxl_get_string_data",
                                       {{{"str", stringType}}, stringType});
    _functionRegistry.registerInternal("pxl_concat_string",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           stringType
                                       });
    _functionRegistry.registerInternal("pxl_char_at",
                                       {
                                           {
                                               {"str", stringType},
                                               {"index", intType}
                                           },
                                           intType
                                       });
    _functionRegistry.registerInternal("pxl_string_equals",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
    _functionRegistry.registerInternal("pxl_string_not_equals",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
    _functionRegistry.registerInternal("pxl_string_greater",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
    _functionRegistry.registerInternal("pxl_string_smaller",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
    _functionRegistry.registerInternal("pxl_string_greater_equals",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
    _functionRegistry.registerInternal("pxl_string_smaller_equals",
                                       {
                                           {
                                               {"a", stringType},
                                               {"b", stringType}
                                           },
                                           boolType
                                       });
}


void Compiler::initGlobals() {
    _globalRegistry.registerGlobal("mouseX", _typeContext.getFloat(), "pxl_mouse_x");
    _globalRegistry.registerGlobal("mouseY", _typeContext.getFloat(), "pxl_mouse_y");
    _globalRegistry.registerGlobal("mousePressed", _typeContext.getBool(), "pxl_mouse_pressed");
    _globalRegistry.registerGlobal("keyPressed", _typeContext.getBool(), "pxl_key_pressed");
    _globalRegistry.registerGlobal("keyCode", _typeContext.getInt(), "pxl_key_code");
}
