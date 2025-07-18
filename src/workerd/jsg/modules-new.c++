// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "modules-new.h"

#include "buffersource.h"

#include <workerd/jsg/function.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/util.h>

#include <kj/mutex.h>
#include <kj/table.h>

namespace workerd::jsg::modules {

namespace {
// Returns kj::none if this given module is incapable of resolving the given
// context. Otherwise, returns the module.
kj::Maybe<const Module&> checkModule(const ResolveContext& context, const Module& module) {
  if (!module.evaluateContext(context)) {
    return kj::none;
  }
  return module;
};

// Ensure that the given module has been instantiated or errored.
// If false is returned, then an exception should have been scheduled
// on the isolate.
bool ensureInstantiated(Lock& js,
    v8::Local<v8::Module> module,
    const CompilationObserver& observer,
    const Module& self) {
  return module->GetStatus() != v8::Module::kUninstantiated ||
      self.instantiate(js, module, observer);
}

constexpr ResolveContext::Type moduleTypeToResolveContextType(Module::Type type) {
  switch (type) {
    case Module::Type::BUNDLE: {
      return ResolveContext::Type::BUNDLE;
    }
    case Module::Type::BUILTIN: {
      return ResolveContext::Type::BUILTIN;
    }
    case Module::Type::BUILTIN_ONLY: {
      return ResolveContext::Type::BUILTIN_ONLY;
    }
    case Module::Type::FALLBACK: {
      return ResolveContext::Type::BUNDLE;
    }
  }
  KJ_UNREACHABLE;
}

constexpr ModuleBundle::Type toModuleBuilderType(ModuleBundle::BuiltinBuilder::Type type) {
  switch (type) {
    case ModuleBundle::BuiltinBuilder::Type::BUILTIN:
      return ModuleBundle::Type::BUILTIN;
    case ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY:
      return ModuleBundle::Type::BUILTIN_ONLY;
  }
  KJ_UNREACHABLE;
}

// The implementation of Module for ESM.
class EsModule final: public Module {
 public:
  explicit EsModule(Url specifier, Type type, Flags flags, kj::ArrayPtr<const char> source)
      : Module(kj::mv(specifier), type, flags | Flags::ESM | Flags::EVAL),
        source(source),
        cachedData(kj::none) {
    KJ_DASSERT(isEsm());
  }
  // This variation does not take ownership of the source buffer.
  KJ_DISALLOW_COPY_AND_MOVE(EsModule);

  v8::MaybeLocal<v8::Module> getDescriptor(
      Lock& js, const CompilationObserver& observer) const override {
    auto metrics = observer.onEsmCompilationStart(js.v8Isolate, kj::str(specifier().getHref()),
        type() == Type::BUNDLE ? CompilationObserver::Option::BUNDLE
                               : CompilationObserver::Option::BUILTIN);

    static constexpr int resourceLineOffset = 0;
    static constexpr int resourceColumnOffset = 0;
    static constexpr bool resourceIsSharedCrossOrigin = false;
    static constexpr int scriptId = -1;
    static constexpr bool resourceIsOpaque = false;
    static constexpr bool isWasm = false;
    v8::ScriptOrigin origin(js.str(specifier().getHref()), resourceLineOffset, resourceColumnOffset,
        resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque, isWasm, true);

    auto options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;

    v8::Local<v8::Module> module;
    {
      v8::ScriptCompiler::CachedData* data = nullptr;

      // Check to see if we have cached compilation data for this module.
      // Importantly, we want to allow multiple threads to be capable of
      // reading and using the cached data without blocking each other
      // (which is fine since using the cache does not modify it).
      auto lock = cachedData.lockShared();
      KJ_IF_SOME(c, *lock) {
        // We new new here because v8 will take ownership of the CachedData instance,
        // even tho we are maintaining ownership of the underlying buffer.
        data = new v8::ScriptCompiler::CachedData(
            c->data, c->length, v8::ScriptCompiler::CachedData::BufferPolicy::BufferNotOwned);
        auto check = data->CompatibilityCheck(js.v8Isolate);
        if (check != v8::ScriptCompiler::CachedData::kSuccess) {
          // The cached data is not compatible with the current isolate. Let's
          // not try using it.
          delete data;
        } else {
          observer.onCompileCacheFound(js.v8Isolate);
        }
      }

      // Note that the Source takes ownership of the CachedData pointer that we pass in.
      // (but not the actual buffer it holds). Do not use data after this point.
      v8::ScriptCompiler::Source source(js.strExtern(this->source), origin, data);

      auto maybeCached = source.GetCachedData();
      if (maybeCached != nullptr) {
        if (!maybeCached->rejected) {
          // We found valid cached data and set the option to consume it to avoid
          // compiling again below...
          options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
        } else {
          // In this case we'll just log a warning and continue on. This is potentially
          // a signal that something with the compile cache is not working correctly but
          // it is not a fatal error. If we spot this in the wild, it warrants some
          // investigation but is not critical.
          LOG_WARNING_ONCE("NOSENTRY Cached data for an ESM module was rejected");
          observer.onCompileCacheRejected(js.v8Isolate);
        }
      }

      // Let's just double check that our options are valid. They should be
      // since we're either consuming cached data or not using any options at all.
      KJ_ASSERT(v8::ScriptCompiler::CompileOptionsIsValid(options));
      if (!v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, options).ToLocal(&module)) {
        return v8::MaybeLocal<v8::Module>();
      }
    }

    // If options is still kNoCompileOptions at this point, it means that we did not
    // find any cached data for this module, or the cached data was rejected. In the
    // case it was rejected, we just move on. If there is no cached data, we try
    // generating it and store it. Multiple threads can end up lining up here to
    // acquire the lock and generate the cache. We'll test to see if the cached
    // data is still empty once the lock is acquired, and if it is not, we'll skip
    // generation.
    if (options == v8::ScriptCompiler::CompileOptions::kNoCompileOptions) {
      auto lock = cachedData.lockExclusive();
      if (*lock == kj::none) {
        if (auto ptr = v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript())) {
          kj::Own<v8::ScriptCompiler::CachedData> cached(
              ptr, kj::_::HeapDisposer<v8::ScriptCompiler::CachedData>::instance);
          *lock = kj::mv(cached);
          observer.onCompileCacheGenerated(js.v8Isolate);
        } else {
          observer.onCompileCacheGenerationFailed(js.v8Isolate);
        }
      }
    }

    return module;
  }

 private:
  v8::MaybeLocal<v8::Value> actuallyEvaluate(
      Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const override {
    return module->Evaluate(js.v8Context());
  }

  v8::MaybeLocal<v8::Value> evaluate(Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer,
      kj::Maybe<EvalCallback>& maybeEvalCallback) const override {
    if (!ensureInstantiated(js, module, observer, *this)) {
      return v8::MaybeLocal<v8::Value>();
    };

    // No need to check isEval here since EsModules are always eval'd.
    KJ_IF_SOME(evalCallback, maybeEvalCallback) {
      return js.wrapSimplePromise(evalCallback(js, *this, module, observer));
    }

    return actuallyEvaluate(js, module, observer);
  }

  kj::ArrayPtr<const char> source;
  // When externed is true, the source buffer is passed into the isolate as an externalized
  // string. This is only appropriate for built-in modules that are compiled into the binary.
  kj::MutexGuarded<kj::Maybe<kj::Own<v8::ScriptCompiler::CachedData>>> cachedData;
};

// A SyntheticModule is essentially any type of module that is not backed by an ESM
// script. More specifically, it's a module in which we synthetically construct the
// module namespace (i.e. the exports) and the evaluation steps. This is used for things
// like CommonJS modules, JSON modules, etc.
class SyntheticModule final: public Module {
 public:
  // The name of the default export.
  static constexpr auto DEFAULT = "default"_kjc;

  SyntheticModule(Url specifier,
      Type type,
      ModuleBundle::BundleBuilder::EvaluateCallback callback,
      kj::Array<kj::String> namedExports,
      Flags flags = Flags::NONE)
      : Module(kj::mv(specifier), type, flags),
        callback(kj::mv(callback)),
        namedExports(kj::mv(namedExports)) {
    // Synthetic modules can never be ESM or Main
    KJ_DASSERT(!isEsm() && !isMain());
  }

  v8::MaybeLocal<v8::Module> getDescriptor(Lock& js, const CompilationObserver&) const override {
    // We add one to the size to accomodate the default export.
    v8::LocalVector<v8::String> exports(js.v8Isolate, namedExports.size() + 1);
    int n = 0;
    exports[n++] = js.strIntern(DEFAULT);
    for (const auto& exp: namedExports) {
      exports[n++] = js.strIntern(exp);
    }
    return v8::Module::CreateSyntheticModule(js.v8Isolate, js.str(specifier().getHref()),
        v8::MemorySpan<const v8::Local<v8::String>>(exports.data(), exports.size()),
        evaluationSteps);
  }

 private:
  static v8::MaybeLocal<v8::Value> evaluationSteps(
      v8::Local<v8::Context> context, v8::Local<v8::Module> module);

  v8::MaybeLocal<v8::Value> actuallyEvaluate(
      Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const override {
    // The return value will be a resolved promise.
    v8::Local<v8::Promise::Resolver> resolver;

    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver)) {
      return v8::MaybeLocal<v8::Value>();
    }

    ModuleNamespace ns(module, namedExports);
    if (!const_cast<SyntheticModule*>(this)->callback(js, specifier(), ns, observer)) {
      // An exception should already be scheduled with the isolate
      return v8::MaybeLocal<v8::Value>();
    }

    if (resolver->Resolve(js.v8Context(), js.v8Undefined()).IsNothing()) {
      return v8::MaybeLocal<v8::Value>();
    }

    return resolver->GetPromise();
  }

  v8::MaybeLocal<v8::Value> evaluate(Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer,
      kj::Maybe<EvalCallback>& maybeEvalCallback) const override {
    if (!ensureInstantiated(js, module, observer, *this)) {
      return v8::MaybeLocal<v8::Value>();
    }

    // If this synthetic module is marked with Flags::EVAL, and the evalCallback
    // is specified, then we defer evaluation to the given callback.
    if (isEval()) {
      KJ_IF_SOME(evalCallback, maybeEvalCallback) {
        return js.wrapSimplePromise(evalCallback(js, *this, module, observer));
      }
    }

    return actuallyEvaluate(js, module, observer);
  }

  ModuleBundle::BundleBuilder::EvaluateCallback callback;
  kj::Array<kj::String> namedExports;
};

// Binds a ModuleRegistry to an Isolate.
class IsolateModuleRegistry final {
 public:
  static IsolateModuleRegistry& from(v8::Isolate* isolate) {
    auto context = isolate->GetCurrentContext();
    void* ptr = context->GetAlignedPointerFromEmbedderData(2);
    KJ_ASSERT(ptr != nullptr);
    return *static_cast<IsolateModuleRegistry*>(ptr);
  }

  struct SpecifierContext final {
    ResolveContext::Type type;
    Url specifier;
    SpecifierContext(const ResolveContext& resolveContext)
        : type(resolveContext.type),
          specifier(resolveContext.specifier.clone()) {}
    bool operator==(const SpecifierContext& other) const {
      return type == other.type && specifier == other.specifier;
    }
    uint hashCode() const {
      return kj::hashCode(type, specifier);
    }
  };

  struct Entry final {
    HashableV8Ref<v8::Module> key;
    SpecifierContext specifier;
    const Module& module;
  };

  IsolateModuleRegistry(Lock& js, ModuleRegistry& registry, const CompilationObserver& observer);
  KJ_DISALLOW_COPY_AND_MOVE(IsolateModuleRegistry);

  // Used to implement the normal static import of modules (using `import ... from`).
  // Returns the v8::Module descriptor. If an empty v8::MaybeLocal is returned, then
  // an exception has been scheduled with the isolate.
  v8::MaybeLocal<v8::Module> resolve(Lock& js, const ResolveContext& context) {
    // Do we already have a cached module for this context?
    KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
      return found.key.getHandle(js);
    }
    // No? That's OK, let's look it up.
    KJ_IF_SOME(found, resolveWithCaching(js, context)) {
      return found.key.getHandle(js);
    }

    // Nothing found? Aw... fail!
    JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", context.specifier.getHref()));
  }

  // Used to implement the async dynamic import of modules (using `await import(...)`)
  // Returns a promise that is resolved once the module is resolved. If any empty
  // v8::MaybeLocal is returned, then an exception has been scheduled with the isolate.
  v8::MaybeLocal<v8::Promise> dynamicResolve(
      Lock& js, Url specifier, Url referrer, kj::StringPtr rawSpecifier) {
    static constexpr auto evaluate = [](Lock& js, Entry& entry, const CompilationObserver& observer,
                                         kj::Maybe<Module::EvalCallback>& maybeEvalCallback) {
      auto module = entry.key.getHandle(js);
      return js
          .toPromise(check(entry.module.evaluate(js, module, observer, maybeEvalCallback))
                         .As<v8::Promise>())
          .then(js, [module = js.v8Ref(module)](Lock& js, Value) mutable -> Promise<Value> {
        return js.resolvedPromise(js.v8Ref(module.getHandle(js)->GetModuleNamespace()));
      });
    };

    return js.wrapSimplePromise(js.tryCatch([&]() -> Promise<Value> {
      // The referrer should absolutely already be known to the registry
      // or something bad happened.
      auto& referring = JSG_REQUIRE_NONNULL(lookupCache.find<kj::HashIndex<UrlCallbacks>>(referrer),
          TypeError, kj::str("Referring module not found in the registry: ", referrer.getHref()));

      // Now that we know the referrer module, we can set the context for the
      // next resolve. In particular, the "type" of the context is determine
      // by the type of the referring module.
      ResolveContext context = {
        .type = moduleTypeToResolveContextType(referring.module.type()),
        .source = ResolveContext::Source::DYNAMIC_IMPORT,
        .specifier = specifier,
        .referrer = referrer,
        .rawSpecifier = rawSpecifier,
      };

      // Do we already have a cached module for this context?
      KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
        return evaluate(js, found, getObserver(), inner.getEvalCallback());
      }

      // No? That's OK, let's look it up.
      KJ_IF_SOME(found, resolveWithCaching(js, context)) {
        return evaluate(js, found, getObserver(), inner.getEvalCallback());
      }

      // Nothing found? Aw... fail!
      JSG_FAIL_REQUIRE(TypeError, kj::str("Module not found: ", specifier.getHref()));
    }, [&](Value exception) -> Promise<Value> {
      return js.rejectedPromise<Value>(kj::mv(exception));
    }));
  }

  enum class RequireOption {
    DEFAULT,
    RETURN_EMPTY,
  };

  // Used to implement the synchronous dynamic import of modules in support of APIs
  // like the CommonJS require. Returns the instantiated/evaluated module namespace.
  // If an empty v8::MaybeLocal is returned and the default option is given, then an
  // exception has been scheduled.
  // Note that this returns the module namespace object. In CommonJS, the require()
  // function will actually return the default export from the module namespace object.
  v8::MaybeLocal<v8::Object> require(
      Lock& js, const ResolveContext& context, RequireOption option = RequireOption::DEFAULT) {
    static constexpr auto evaluate = [](Lock& js, Entry& entry, const Url& specifier,
                                         const CompilationObserver& observer,
                                         kj::Maybe<Module::EvalCallback>& maybeEvalCallback) {
      auto module = entry.key.getHandle(js);
      auto status = module->GetStatus();

      // If status is kErrored, that means a prior attempt to evaluate the module
      // failed. We simply propagate the same error here.
      if (status == v8::Module::kErrored) {
        js.throwException(JsValue(module->GetException()));
      }

      // Circular dependencies should be fine when we are talking strictly
      // about CJS/Node.js style modules. For ESM, it becomes more problematic
      // because v8 will not allow us to grab the default export while the module
      // is still evaluating.

      if (entry.module.isEsm() && status == v8::Module::kEvaluating) {
        JSG_FAIL_REQUIRE(Error, "Circular dependency when resolving module: ", specifier);
      }

      // If the module has already been evaluated, or is in the process of being
      // evaluated, return the module namespace object directly. Note that if the
      // module is a synthetic module, and status is kEvaluating, it is possible
      // and likely that the namespace has not yet been fully evaluated and will
      // be incomplete here. This allows CJS circular dependencies to be supported
      // to a degree. Just like in Node.js, however, such circular dependencies
      // can still be problematic depending on how they are used.
      if (status == v8::Module::kEvaluated || status == v8::Module::kEvaluating) {
        return module->GetModuleNamespace().As<v8::Object>();
      }

      // Evaluate the module and grab the default export from the module namespace.
      auto promise =
          check(entry.module.evaluate(js, module, observer, maybeEvalCallback)).As<v8::Promise>();

      // Run the microtasks to ensure that any promises that happen to be scheduled
      // during the evaluation of the top-level scope have a chance to be settled,
      js.runMicrotasks();

      static const auto kTopLevelAwaitError =
          "Use of top-level await in a synchronously required module is restricted to "
          "promises that are resolved synchronously. This includes any top-level awaits "
          "in the entrypoint module for a worker."_kj;

      switch (promise->State()) {
        case v8::Promise::kFulfilled: {
          // This is what we want. The module namespace should be fully populated
          // and evaluated at this point.
          return module->GetModuleNamespace().As<v8::Object>();
        }
        case v8::Promise::kRejected: {
          // Oops, there was an error. We should throw it.
          js.throwException(JsValue(promise->Result()));
          break;
        }
        case v8::Promise::kPending: {
          // The module evaluation could not complete in a single drain of the
          // microtask queue. This means we've got a pending promise somwwhere
          // that is being awaited preventing the module from being ready to
          // go. We can't have that! Throw! Throw!
          JSG_FAIL_REQUIRE(Error, kTopLevelAwaitError, " Specifier: \"", specifier, "\".");
        }
      }
      KJ_UNREACHABLE;
    };

    return js.tryCatch([&]() -> v8::MaybeLocal<v8::Object> {
      // Do we already have a cached module for this context?
      KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
        return evaluate(js, found, context.specifier, getObserver(), inner.getEvalCallback());
      }

      KJ_IF_SOME(found, resolveWithCaching(js, context)) {
        return evaluate(js, found, context.specifier, getObserver(), inner.getEvalCallback());
      }

      if (option == RequireOption::RETURN_EMPTY) {
        return v8::MaybeLocal<v8::Object>();
      }
      JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", context.specifier.getHref()));
    }, [&](Value exception) {
      // Use the isolate to rethrow the exception here instead of using the lock.
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return v8::MaybeLocal<v8::Object>();
    });
  }

  // Lookup a module that may have already been previously resolved and cached.
  kj::Maybe<Entry&> lookup(Lock& js, v8::Local<v8::Module> module) {
    return lookupCache
        .find<kj::HashIndex<EntryCallbacks>>(HashableV8Ref<v8::Module>(js.v8Isolate, module))
        .map([](Entry& entry) -> Entry& { return entry; });
  }

  const jsg::Url& getBundleBase() const {
    return inner.getBundleBase();
  }

 private:
  ModuleRegistry& inner;
  const CompilationObserver& observer;

  const CompilationObserver& getObserver() const {
    return observer;
  }

  struct EntryCallbacks final {
    const HashableV8Ref<v8::Module>& keyForRow(const Entry& entry) const {
      return entry.key;
    }
    bool matches(const Entry& entry, const HashableV8Ref<v8::Module>& key) const {
      return entry.key == key;
    }
    uint hashCode(const HashableV8Ref<v8::Module>& ref) const {
      return ref.hashCode();
    }
  };

  struct ContextCallbacks final {
    const SpecifierContext& keyForRow(const Entry& entry) const {
      return entry.specifier;
    }
    bool matches(const Entry& entry, const SpecifierContext& specifier) const {
      return entry.specifier == specifier;
    }
    uint hashCode(const SpecifierContext& specifier) const {
      return specifier.hashCode();
    }
  };

  struct UrlCallbacks final {
    const Url& keyForRow(const Entry& entry) const {
      return entry.specifier.specifier;
    }
    bool matches(const Entry& entry, const Url& specifier) const {
      return entry.specifier.specifier == specifier;
    }
    uint hashCode(const Url& specifier) const {
      return specifier.hashCode();
    }
  };

  // Resolves the module from the inner ModuleRegistry, caching the results.
  kj::Maybe<Entry&> resolveWithCaching(
      Lock& js, const ResolveContext& context) KJ_WARN_UNUSED_RESULT {
    ResolveContext innerContext{
      // The type identifies the resolution context as a bundle, builtin, or builtin-only.
      .type = context.type,
      // The source identifies the method of resolution (static import, dynamic import, etc).
      // This is passed along for informational purposes only.
      .source = context.source,
      // The inner registry should ignore all URL query parameters and fragments
      .specifier = context.specifier.clone(
          Url::EquivalenceOption::IGNORE_FRAGMENTS | Url::EquivalenceOption::IGNORE_SEARCH),
      // The referrer is passed along for informational purposes only.
      .referrer = context.referrer,
    };
    KJ_IF_SOME(found, inner.resolve(innerContext)) {
      return kj::Maybe<Entry&>(lookupCache.upsert(
          Entry{
            .key = HashableV8Ref<v8::Module>(
                js.v8Isolate, check(found.getDescriptor(js, getObserver()))),
            // Note that we cache specifically with the passed in context and not the
            // innerContext that was created. This is because we want to use the original
            // specifier URL (with query parameters and fragments) as part of the key for
            // the lookup cache.
            .specifier = context,
            .module = found,
          },
          [](auto&, auto&&) {}));
    }
    return kj::none;
  }

  kj::Table<Entry,
      kj::HashIndex<EntryCallbacks>,
      kj::HashIndex<ContextCallbacks>,
      kj::HashIndex<UrlCallbacks>>
      lookupCache;
  friend class SyntheticModule;
};

v8::MaybeLocal<v8::Value> SyntheticModule::evaluationSteps(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
  try {
    auto& js = Lock::current();
    auto& registry = IsolateModuleRegistry::from(js.v8Isolate);

    KJ_IF_SOME(found, registry.lookup(js, module)) {
      return found.module.evaluate(
          js, module, registry.getObserver(), registry.inner.getEvalCallback());
    }

    // This case really should never actually happen but we handle it anyway.
    KJ_LOG(ERROR, "Synthetic module not found in registry for evaluation");

    js.v8Isolate->ThrowError(js.str("Requested module does not exist"_kj));
    return v8::MaybeLocal<v8::Value>();
  } catch (...) {
    kj::throwFatalException(kj::getCaughtExceptionAsKj());
  }
}

// Set up the special `import.meta` property for the module.
void importMeta(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module, v8::Local<v8::Object> meta) {
  auto& js = Lock::current();
  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  try {
    js.tryCatch([&] {
      KJ_IF_SOME(found, registry.lookup(js, module)) {
        auto href = found.specifier.specifier.getHref();

        // V8's documentation says that the host should set the properties
        // using CreateDataProperty.

        if (meta->CreateDataProperty(js.v8Context(), v8::Local<v8::String>(js.strIntern("main"_kj)),
                    js.boolean(found.module.isMain()))
                .IsNothing()) {
          // Notice that we do not use check here. There should be an exception
          // scheduled with the isolate, it will take care of it at this point.
          return;
        }

        if (meta->CreateDataProperty(
                    js.v8Context(), v8::Local<v8::String>(js.strIntern("url"_kj)), js.str(href))
                .IsNothing()) {
          return;
        }

        // The import.meta.resolve(...) function is effectively a shortcut for
        // new URL(specifier, import.meta.url).href. The idea is that it allows
        // resolving import specifiers relative to the current modules base URL.
        // Note that we do not validate that the resolved URL actually matches
        // anything in the registry.
        auto resolve = js.wrapReturningFunction(js.v8Context(),
            [href = kj::mv(href)](
                Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) -> JsValue {
          // Note that we intentionally use ToString here to coerce whatever value is given
          // into a string or throw if it cannot be coerced.
          auto specifier = js.toString(args[0]);
          KJ_IF_SOME(resolved, Url::tryParse(specifier.asPtr(), href)) {
            auto normalized = resolved.clone(Url::EquivalenceOption::NORMALIZE_PATH);
            return js.str(normalized.getHref());
          } else {
            // If the specifier could not be parsed and resolved successfully,
            // the spec says to return null.
            return js.null();
          }
        });

        if (meta->CreateDataProperty(
                    js.v8Context(), v8::Local<v8::String>(js.strIntern("resolve"_kj)), resolve)
                .IsNothing()) {
          return;
        }
      }
    }, [&](Value exception) {
      // It would be exceedingly odd to end up here but we handle it anyway,
      // just to ensure that we do not crash the isolate. The only thing we'll
      // do is rethrow the error.
      js.v8Isolate->ThrowException(exception.getHandle(js));
    });
  } catch (...) {
    kj::throwFatalException(kj::getCaughtExceptionAsKj());
  }
}

// The callback v8 calls when dynamic import(...) is used.
v8::MaybeLocal<v8::Promise> dynamicImport(v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes) {
  auto& js = Lock::current();

  // Since this method is called directly by V8, we don't want to use jsg::check
  // or the js.rejectedPromise variants since those can throw JsExceptionThrown.
  constexpr static auto rejected = [](jsg::Lock& js,
                                       const jsg::JsValue& error) -> v8::MaybeLocal<v8::Promise> {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver) ||
        resolver->Reject(js.v8Context(), error).IsNothing()) {
      return v8::MaybeLocal<v8::Promise>();
    }
    return resolver->GetPromise();
  };

  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  try {
    return js.tryCatch([&]() -> v8::MaybeLocal<v8::Promise> {
      auto spec = js.toString(specifier);

      // The proposed specification for import attributes strongly recommends that
      // embedders reject import attributes and types they do not understand/implement.
      // This is because import attributes can alter the interpretation of a module.
      // Throwing an error for things we do not understand is the safest thing to do
      // for backwards compatibility.
      //
      // For now, we do not support any import attributes, so if there are any at all
      // we will reject the import.
      if (!import_attributes.IsEmpty() && import_attributes->Length() > 0) {
        return rejected(js, js.typeError("Import attributes are not supported"));
      };

      Url referrer = ([&] {
        if (resource_name.IsEmpty()) {
          return registry.getBundleBase().clone();
        }
        auto str = js.toString(resource_name);
        return KJ_ASSERT_NONNULL(Url::tryParse(str.asPtr()));
      })();

      // If Node.js Compat v2 mode is enable, we have to check to see if the specifier
      // is a bare node specifier and resolve it to a full node: URL.
      if (isNodeJsCompatEnabled(js)) {
        KJ_IF_SOME(nodeSpec, checkNodeSpecifier(spec)) {
          spec = kj::mv(nodeSpec);
        }
      }

      // Handle process module redirection based on enable_nodejs_process_v2 flag
      if (spec == "node:process") {
        auto processSpec = isNodeJsProcessV2Enabled(js) ? "node-internal:public_process"_kj
                                                        : "node-internal:legacy_process"_kj;
        KJ_IF_SOME(url, referrer.tryResolve(processSpec)) {
          auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
          ResolveContext context = {
            .type = ResolveContext::Type::BUILTIN_ONLY,
            .source = ResolveContext::Source::DYNAMIC_IMPORT,
            .specifier = normalized,
            .referrer = referrer,
            .rawSpecifier = processSpec,
          };
          return registry.dynamicResolve(js, kj::mv(normalized), kj::mv(referrer), processSpec);
        }
      }

      KJ_IF_SOME(url, referrer.tryResolve(spec.asPtr())) {
        auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
        return registry.dynamicResolve(js, kj::mv(normalized), kj::mv(referrer), spec);
      }

      // We were not able to parse the specifier. We'll return a rejected promise.
      return rejected(js, js.typeError(kj::str("Invalid module specifier: ", spec)));
    }, [&](Value exception) -> v8::MaybeLocal<v8::Promise> {
      // If there are any synchronously thrown exceptions, we want to catch them
      // here and convert them into a rejected promise. The only exception are
      // fatal cases where the isolate is terminating which won't make it here
      // anyway.
      return rejected(js, jsg::JsValue(exception.getHandle(js)));
    });
  } catch (...) {
    kj::throwFatalException(kj::getCaughtExceptionAsKj());
  }
}

IsolateModuleRegistry::IsolateModuleRegistry(
    Lock& js, ModuleRegistry& registry, const CompilationObserver& observer)
    : inner(registry),
      observer(observer),
      lookupCache(EntryCallbacks{}, ContextCallbacks{}, UrlCallbacks{}) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  KJ_ASSERT(!context.IsEmpty());
  context->SetAlignedPointerInEmbedderData(2, this);
  isolate->SetHostImportModuleDynamicallyCallback(&dynamicImport);
  isolate->SetHostInitializeImportMetaObjectCallback(&importMeta);
}

// The callback v8 calls when static import is used.
v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes,
    v8::Local<v8::Module> referrer) {
  auto& js = Lock::current();
  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);

  return js.tryCatch([&]() -> v8::MaybeLocal<v8::Module> {
    auto spec = kj::str(specifier);

    // The proposed specification for import attributes strongly recommends that
    // embedders reject import attributes and types they do not understand/implement.
    // This is because import attributes can alter the interpretation of a module.
    // Throwing an error for things we do not understand is the safest thing to do
    // for backwards compatibility.
    //
    // For now, we do not support any import attributes, so if there are any at all
    // we will reject the import.
    if (!import_attributes.IsEmpty() && import_attributes->Length() > 0) {
      js.throwException(js.typeError("Import attributes are not supported"));
    }

    ResolveContext::Type type = ResolveContext::Type::BUNDLE;

    auto& referrerUrl = registry.lookup(js, referrer)
                            .map([&](IsolateModuleRegistry::Entry& entry) -> const Url& {
      type = moduleTypeToResolveContextType(entry.module.type());
      return entry.specifier.specifier;
    }).orDefault(registry.getBundleBase());

    // If Node.js Compat v2 mode is enable, we have to check to see if the specifier
    // is a bare node specifier and resolve it to a full node: URL.
    if (isNodeJsCompatEnabled(js)) {
      KJ_IF_SOME(nodeSpec, checkNodeSpecifier(spec)) {
        spec = kj::mv(nodeSpec);
      }
    }

    // Handle process module redirection based on enable_nodejs_process_v2 flag
    if (spec == "node:process") {
      auto processSpec = isNodeJsProcessV2Enabled(js) ? "node-internal:public_process"_kj
                                                      : "node-internal:legacy_process"_kj;
      KJ_IF_SOME(url, referrerUrl.tryResolve(processSpec)) {
        auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
        ResolveContext resolveContext = {
          .type = ResolveContext::Type::BUILTIN_ONLY,
          .source = ResolveContext::Source::STATIC_IMPORT,
          .specifier = normalized,
          .referrer = referrerUrl,
          .rawSpecifier = processSpec,
        };
        return registry.resolve(js, resolveContext);
      }
    }

    KJ_IF_SOME(url, referrerUrl.tryResolve(spec)) {
      // Make sure that percent-encoding in the path is normalized so we can match correctly.
      auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
      ResolveContext resolveContext = {
        .type = type,
        .source = ResolveContext::Source::STATIC_IMPORT,
        .specifier = normalized,
        .referrer = referrerUrl,
        .rawSpecifier = spec.asPtr(),
      };
      // TODO(soon): Add import attributes to the context.

      return registry.resolve(js, resolveContext);
    }

    js.throwException(js.error(kj::str("Invalid module specifier: "_kj, specifier)));
  }, [&](Value exception) -> v8::MaybeLocal<v8::Module> {
    // If there are any synchronously thrown exceptions, we want to catch them
    // here and convert them into a rejected promise. The only exception are
    // fatal cases where the isolate is terminating which won't make it here
    // anyway.
    js.v8Isolate->ThrowException(exception.getHandle(js));
    return v8::MaybeLocal<v8::Module>();
  });
}

// The fallback module bundle calls a single resolve callback to resolve all modules
// it is asked to resolve. Instances must be thread-safe.
class FallbackModuleBundle final: public ModuleBundle {
 public:
  FallbackModuleBundle(Builder::ResolveCallback&& callback)
      : ModuleBundle(Type::FALLBACK),
        callback(kj::mv(callback)) {}

  kj::Maybe<Resolved> resolve(const ResolveContext& context) override {
    {
      auto lock = cache.lockShared();
      KJ_IF_SOME(found, lock->storage.find(context.specifier)) {
        const Module& module = *found;
        return Resolved{
          .module = module,
        };
      }
      KJ_IF_SOME(found, lock->aliases.find(context.specifier)) {
        return Resolved{
          .module = *found,
        };
      }
    }

    {
      auto lock = cache.lockExclusive();
      KJ_IF_SOME(resolved, callback(context)) {
        KJ_SWITCH_ONEOF(resolved) {
          KJ_CASE_ONEOF(str, kj::String) {
            return Resolved{
              .specifier = kj::mv(str),
            };
          }
          KJ_CASE_ONEOF(resolved, kj::Own<Module>) {
            Module& module = *resolved;
            lock->storage.upsert(context.specifier.clone(), kj::mv(resolved));
            if (module.specifier() != context.specifier) {
              lock->aliases.upsert(module.specifier().clone(), &module);
            }
            return Resolved{
              .module = module,
            };
          }
        }
        KJ_UNREACHABLE;
      }
    }

    return kj::none;
  }

 private:
  Builder::ResolveCallback callback;

  struct Cache final {
    kj::HashMap<Url, kj::Own<Module>> storage;
    kj::HashMap<Url, Module*> aliases;
  };

  kj::MutexGuarded<Cache> cache;
};

// The static module bundle maintains an internal table of specifiers to resolve callbacks
// in memory. Instances must be thread-safe.
class StaticModuleBundle final: public ModuleBundle {
 public:
  StaticModuleBundle(Type type,
      kj::HashMap<Url, ModuleBundle::Builder::ResolveCallback> modules,
      kj::HashMap<Url, Url> aliases)
      : ModuleBundle(type),
        modules(kj::mv(modules)),
        aliases(kj::mv(aliases)) {}

  kj::Maybe<Resolved> resolve(const ResolveContext& context) override {
    KJ_IF_SOME(aliased, aliases.find(context.specifier)) {
      // The specifier is registered as an alias. We need to resolve the alias instead.
      // This is set up to allow for recursive aliases.
      ResolveContext newContext{
        .type = context.type,
        .source = context.source,
        .specifier = aliased,
        .referrer = context.referrer,
        .rawSpecifier = context.rawSpecifier,
      };
      return resolve(newContext);
    }

    auto lock = cache.lockExclusive();
    KJ_IF_SOME(cached, lock->find(context.specifier)) {
      return Resolved{
        .module = checkModule(context, *cached),
      };
    }

    // Module was not cached, try to resolve it.
    KJ_IF_SOME(found, modules.find(context.specifier)) {
      KJ_IF_SOME(resolved, found(context)) {
        KJ_SWITCH_ONEOF(resolved) {
          KJ_CASE_ONEOF(str, kj::String) {
            return Resolved{
              .specifier = kj::mv(str),
            };
          }
          KJ_CASE_ONEOF(resolved, kj::Own<Module>) {
            const Module& module = *resolved;
            lock->upsert(context.specifier.clone(), kj::mv(resolved));
            return Resolved{
              .module = checkModule(context, module),
            };
          }
        }
        KJ_UNREACHABLE;
      }
    }

    return kj::none;
  }

 private:
  kj::HashMap<Url, ModuleBundle::Builder::ResolveCallback> modules;
  kj::HashMap<Url, Url> aliases;
  kj::MutexGuarded<kj::HashMap<Url, kj::Own<Module>>> cache;
};

kj::HashSet<kj::StringPtr> toHashSet(kj::ArrayPtr<const kj::String> arr) {
  kj::HashSet<kj::StringPtr> set;
  set.insertAll(arr);
  // Make sure there is no "default" export listed explicitly in the set.
  set.eraseMatch("default"_kj);
  return kj::mv(set);
}

}  // namespace

// ======================================================================================
kj::Own<ModuleBundle> ModuleBundle::newFallbackBundle(Builder::ResolveCallback callback) {
  return kj::heap<FallbackModuleBundle>(kj::mv(callback));
}

void ModuleBundle::getBuiltInBundleFromCapnp(
    BuiltinBuilder& builder, Bundle::Reader bundle, BuiltInBundleOptions options) {
  auto filter = ([&] {
    switch (builder.type()) {
      case Module::Type::BUILTIN:
        return ModuleType::BUILTIN;
      case Module::Type::BUILTIN_ONLY:
        return ModuleType::INTERNAL;
      case Module::Type::BUNDLE:
        break;
      case Module::Type::FALLBACK:
        break;
    }
    KJ_UNREACHABLE;
  })();

  for (auto module: bundle.getModules()) {
    if (module.getType() == filter) {
      auto specifier = KJ_ASSERT_NONNULL(Url::tryParse(module.getName()));
      switch (module.which()) {
        case workerd::jsg::Module::SRC: {
          builder.addEsm(specifier, module.getSrc().asChars());
          continue;
        }
        case workerd::jsg::Module::WASM: {
          builder.addSynthetic(specifier, Module::newWasmModuleHandler(module.getWasm().asBytes()));
          continue;
        }
        case workerd::jsg::Module::DATA: {
          builder.addSynthetic(specifier, Module::newDataModuleHandler(module.getData().asBytes()));
          continue;
        }
        case workerd::jsg::Module::JSON: {
          builder.addSynthetic(specifier, Module::newJsonModuleHandler(module.getJson().asArray()));
          continue;
        }
      }
      KJ_UNREACHABLE;
    }
  }
}

ModuleBundle::ModuleBundle(Type type): type_(type) {}

ModuleBundle::Builder::Builder(Type type): type_(type) {}

ModuleBundle::Builder& ModuleBundle::Builder::alias(const Url& alias, const Url& specifier) {
  auto aliasNormed = alias.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  if (modules_.find(aliasNormed) != kj::none || aliases_.find(aliasNormed) != kj::none) {
    KJ_FAIL_REQUIRE(kj::str("Module \"", aliasNormed.getHref(), "\" already added to bundle"));
  }
  aliases_.insert(kj::mv(aliasNormed), specifier.clone(Url::EquivalenceOption::NORMALIZE_PATH));
  return *this;
}

ModuleBundle::Builder& ModuleBundle::Builder::add(
    const Url& specifier, Builder::ResolveCallback callback) {
  if (modules_.find(specifier) != kj::none || aliases_.find(specifier) != kj::none) {
    KJ_FAIL_REQUIRE(kj::str("Module \"", specifier.getHref(), "\" already added to bundle"));
  }
  modules_.insert(specifier.clone(), kj::mv(callback));
  return *this;
}

kj::Own<ModuleBundle> ModuleBundle::Builder::finish() {
  return kj::heap<StaticModuleBundle>(type_, kj::mv(modules_), kj::mv(aliases_));
}

void ModuleBundle::Builder::ensureIsNotBundleSpecifier(const Url& specifier) {
  // The file: protocol is reserved for bundle type modules.
  KJ_REQUIRE(specifier.getProtocol() != "file:"_kjc,
      "The file: protocol is reserved for bundle type modules");
}

// ======================================================================================

ModuleBundle::BundleBuilder::BundleBuilder(const jsg::Url& bundleBase)
    : ModuleBundle::Builder(Type::BUNDLE),
      bundleBase(bundleBase) {}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addSyntheticModule(
    kj::StringPtr specifier, EvaluateCallback callback, kj::Array<kj::String> namedExports) {
  auto url = KJ_ASSERT_NONNULL(bundleBase.tryResolve(specifier));
  // Make sure that percent-encoding in the path is normalized so we can match correctly.
  url = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  add(url,
      [url = url.clone(), callback = kj::mv(callback), namedExports = kj::mv(namedExports),
          type = type()](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod =
        Module::newSynthetic(kj::mv(url), type, kj::mv(callback), kj::mv(namedExports));
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addEsmModule(
    kj::StringPtr specifier, kj::ArrayPtr<const char> source, Module::Flags flags) {
  auto url = KJ_ASSERT_NONNULL(bundleBase.tryResolve(specifier));
  // Make sure that percent-encoding in the path is normalized so we can match correctly.
  url = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  add(url,
      [url = url.clone(), source, flags, type = type()](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = kj::heap<EsModule>(kj::mv(url), type, flags, source);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::alias(
    kj::StringPtr alias, kj::StringPtr specifier) {
  auto aliasUrl = KJ_ASSERT_NONNULL(bundleBase.tryResolve(alias));
  auto specifierUrl = KJ_ASSERT_NONNULL(bundleBase.tryResolve(specifier));
  Builder::alias(aliasUrl, specifierUrl);
  return *this;
}

// ======================================================================================

ModuleBundle::BuiltinBuilder::BuiltinBuilder(Type type)
    : ModuleBundle::Builder(toModuleBuilderType(type)) {}

ModuleBundle::BuiltinBuilder& ModuleBundle::BuiltinBuilder::addSynthetic(
    const Url& specifier, ModuleBundle::BundleBuilder::EvaluateCallback callback) {
  ensureIsNotBundleSpecifier(specifier);
  Builder::add(specifier,
      [url = specifier.clone(), callback = kj::mv(callback), type = type()](
          const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newSynthetic(kj::mv(url), type, kj::mv(callback));
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BuiltinBuilder& ModuleBundle::BuiltinBuilder::addEsm(
    const Url& specifier, kj::ArrayPtr<const char> source) {
  ensureIsNotBundleSpecifier(specifier);
  Builder::add(specifier,
      [url = specifier.clone(), source, type = type()](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newEsm(kj::mv(url), type, source);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

// ======================================================================================
ModuleRegistry::Builder::Builder(
    const ResolveObserver& observer, const jsg::Url& bundleBase, Options options)
    : observer(observer),
      bundleBase(bundleBase),
      options(options) {}

bool ModuleRegistry::Builder::allowsFallback() const {
  return (options & Options::ALLOW_FALLBACK) == Options::ALLOW_FALLBACK;
}

ModuleRegistry::Builder& ModuleRegistry::Builder::setParent(ModuleRegistry& parent) {
  maybeParent = parent;
  return *this;
}

ModuleRegistry::Builder& ModuleRegistry::Builder::add(kj::Own<ModuleBundle> bundle) {
  if (!allowsFallback()) {
    KJ_REQUIRE(bundle->type() != ModuleBundle::Type::FALLBACK,
        "Fallback bundle types are not allowed for this registry");
  }
  bundles_[static_cast<int>(bundle->type())].add(kj::mv(bundle));
  return *this;
}

ModuleRegistry::Builder& ModuleRegistry::Builder::setEvalCallback(EvalCallback callback) {
  maybeEvalCallback = kj::mv(callback);
  return *this;
}

kj::Own<ModuleRegistry> ModuleRegistry::Builder::finish() {
  return kj::heap<ModuleRegistry>(this);
}

ModuleRegistry::ModuleRegistry(ModuleRegistry::Builder* builder)
    : observer(builder->observer),
      bundleBase(builder->bundleBase),
      maybeParent(builder->maybeParent) {
  bundles_[kBundle] = builder->bundles_[kBundle].releaseAsArray();
  bundles_[kBuiltin] = builder->bundles_[kBuiltin].releaseAsArray();
  bundles_[kBuiltinOnly] = builder->bundles_[kBuiltinOnly].releaseAsArray();
  bundles_[kFallback] = builder->bundles_[kFallback].releaseAsArray();
  maybeEvalCallback = kj::mv(builder->maybeEvalCallback);
}

kj::Own<void> ModuleRegistry::attachToIsolate(Lock& js, const CompilationObserver& observer) {
  // The IsolateModuleRegistry is attached to the isolate as an embedder data slot.
  // We have to keep it alive for the duration of the v8::Context so we return a
  // kj::Own and store that in the jsg::JsContext
  return kj::heap<IsolateModuleRegistry>(js, *this, observer);
}

kj::Maybe<const Module&> ModuleRegistry::resolve(const ResolveContext& context) {
  auto tryFind = [this](const ResolveContext& context,
                     kj::ArrayPtr<kj::Own<ModuleBundle>> bundles) -> kj::Maybe<const Module&> {
    for (auto& bundle: bundles) {
      KJ_IF_SOME(found, bundle->resolve(context)) {
        KJ_IF_SOME(str, found.specifier) {
          // We received a redirect to another module specifier. Let's
          // start resolution over again with the new specifier... but only
          // if we can successfully parse the specifier as a URL.
          KJ_IF_SOME(specifier, jsg::Url::tryParse(str.asPtr())) {

            kj::HashMap<kj::StringPtr, kj::StringPtr> clonedAttrs;
            for (const auto& [key, value]: context.attributes) {
              clonedAttrs.insert(key, value);
            }

            return resolve(ResolveContext{
              .type = context.type,
              .source = context.source,
              .specifier = kj::mv(specifier),
              .referrer = context.referrer.clone(),
              .rawSpecifier = context.rawSpecifier.map([](auto& str) { return kj::str(str); }),
              .attributes = kj::mv(clonedAttrs),
            });
          }
          return kj::none;
        }
        KJ_IF_SOME(module, found.module) {
          return module;
        }
        KJ_UNREACHABLE;
      }
    }
    return kj::none;
  };

  // If the embedder supports it, collect metrics on what modules were resolved.
  auto metrics = observer.onResolveModule(context.specifier, context.type, context.source);

  switch (context.type) {
    case ResolveContext::Type::BUNDLE: {
      // For bundle resolution, we only use Bundle, Builtin, and Fallback bundles,
      // in that order.
      KJ_IF_SOME(found, tryFind(context, bundles_[kBundle])) {
        metrics->found();
        return found;
      }
      KJ_IF_SOME(found, tryFind(context, bundles_[kBuiltin])) {
        metrics->found();
        return found;
      }
      KJ_IF_SOME(found, tryFind(context, bundles_[kFallback])) {
        metrics->found();
        return found;
      }

      KJ_IF_SOME(parent, maybeParent) {
        return parent.resolve(context);
      }

      metrics->notFound();
      return kj::none;
    }
    case ResolveContext::Type::BUILTIN: {
      // For built-in resolution, we only use builtin and builtin-only bundles.
      KJ_IF_SOME(found, tryFind(context, bundles_[kBuiltin])) {
        metrics->found();
        return found;
      }
      KJ_IF_SOME(found, tryFind(context, bundles_[kBuiltinOnly])) {
        metrics->found();
        return found;
      }

      KJ_IF_SOME(parent, maybeParent) {
        return parent.resolve(context);
      }

      metrics->notFound();
      return kj::none;
    }
    case ResolveContext::Type::BUILTIN_ONLY: {
      // For built-in only resolution, we only use builtin-only bundles.
      KJ_IF_SOME(found, tryFind(context, bundles_[kBuiltinOnly])) {
        metrics->found();
        return found;
      }

      KJ_IF_SOME(parent, maybeParent) {
        return parent.resolve(context);
      }

      metrics->notFound();
      return kj::none;
    }
  }

  metrics->notFound();
  return kj::none;
}

kj::Maybe<JsObject> ModuleRegistry::tryResolveModuleNamespace(Lock& js,
    kj::StringPtr specifier,
    ResolveContext::Type type,
    ResolveContext::Source source,
    kj::Maybe<const Url&> maybeReferrer) {
  auto& bound = IsolateModuleRegistry::from(js.v8Isolate);
  auto url = ([&] {
    KJ_IF_SOME(referrer, maybeReferrer) {
      return KJ_ASSERT_NONNULL(referrer.tryResolve(specifier));
    }
    return KJ_ASSERT_NONNULL(bound.getBundleBase().tryResolve(specifier));
  })();
  auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  ResolveContext context{
    .type = type,
    .source = source,
    .specifier = normalized,
    .referrer = maybeReferrer.orDefault(bound.getBundleBase()),
    .rawSpecifier = specifier,
  };
  v8::TryCatch tryCatch(js.v8Isolate);
  auto ns = bound.require(js, context, IsolateModuleRegistry::RequireOption::RETURN_EMPTY);
  if (tryCatch.HasCaught()) {
    tryCatch.ReThrow();
    throw JsExceptionThrown();
  }
  if (ns.IsEmpty()) return kj::none;
  return JsObject(check(ns));
}

JsValue ModuleRegistry::resolve(Lock& js,
    kj::StringPtr specifier,
    kj::StringPtr exportName,
    ResolveContext::Type type,
    ResolveContext::Source source,
    kj::Maybe<const Url&> maybeReferrer) {
  KJ_IF_SOME(ns, tryResolveModuleNamespace(js, specifier, type, source, maybeReferrer)) {
    return ns.get(js, exportName);
  }
  JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", specifier));
}

// ======================================================================================

Module::Module(Url specifier, Type type, Flags flags)
    : specifier_(kj::mv(specifier)),
      type_(type),
      flags_(flags) {}

bool Module::instantiate(
    Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const {
  if (module->GetStatus() != v8::Module::kUninstantiated) {
    return true;
  }
  // InstantiateModule is one of those methods that returns a Maybe<bool> but
  // never returns Just(false). It either returns Just(true) or an empty Maybe
  // to signal that the instantiation failed. Eventually I would expect V8 to
  // replace the return value with a Maybe<void>.
  return module->InstantiateModule(js.v8Context(), resolveCallback).IsJust();
}

bool Module::isEval() const {
  return (flags_ & Flags::EVAL) == Flags::EVAL;
}

bool Module::isEsm() const {
  return (flags_ & Flags::ESM) == Flags::ESM;
}

bool Module::isMain() const {
  return (flags_ & Flags::MAIN) == Flags::MAIN;
}

bool Module::evaluateContext(const ResolveContext& context) const {
  if (context.specifier != specifier()) return false;
  // TODO(soon): Check the import attributes in the context.
  return true;
}

kj::Own<Module> Module::newSynthetic(Url specifier,
    Type type,
    EvaluateCallback callback,
    kj::Array<kj::String> namedExports,
    Flags flags) {
  return kj::heap<SyntheticModule>(
      kj::mv(specifier), type, kj::mv(callback), kj::mv(namedExports), flags);
}

kj::Own<Module> Module::newEsm(Url specifier, Type type, kj::Array<const char> code, Flags flags) {
  return kj::heap<EsModule>(kj::mv(specifier), type, flags, code).attach(kj::mv(code));
}

kj::Own<Module> Module::newEsm(Url specifier, Type type, kj::ArrayPtr<const char> code) {
  return kj::heap<EsModule>(kj::mv(specifier), type, Flags::ESM, code);
}

Module::ModuleNamespace::ModuleNamespace(
    v8::Local<v8::Module> inner, kj::ArrayPtr<const kj::String> namedExports)
    : inner(inner),
      namedExports(toHashSet(namedExports)) {}

bool Module::ModuleNamespace::set(Lock& js, kj::StringPtr name, JsValue value) const {
  if (name != "default"_kj) {
    KJ_REQUIRE(namedExports.find(name) != kj::none, kj::str("Module does not export ", name));
  }

  bool result;
  if (!inner->SetSyntheticModuleExport(js.v8Isolate, js.strIntern(name), value).To(&result)) {
    return false;
  }
  if (!result) {
    js.v8Isolate->ThrowError(js.str(kj::str("Failed to set synthetic module export ", name)));
  }
  return result;
}

bool Module::ModuleNamespace::setDefault(Lock& js, JsValue value) const {
  return set(js, SyntheticModule::DEFAULT, value);
}

kj::ArrayPtr<const kj::StringPtr> Module::ModuleNamespace::getNamedExports() const {
  return kj::ArrayPtr<const kj::StringPtr>(namedExports.begin(), namedExports.size());
}

// ======================================================================================
// Methods to create evaluation callbacks for common synthetic module types. It is
// important to remember that evaluation callbacks can be called multiple times and
// from multiple threads. The callbacks must be thread-safe and idempotent.

Module::EvaluateCallback Module::newTextModuleHandler(kj::ArrayPtr<const char> data) {
  return [data](Lock& js, const Url& specifier, const ModuleNamespace& ns,
             const CompilationObserver&) -> bool {
    return js.tryCatch([&] { return ns.setDefault(js, js.str(data)); }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Module::EvaluateCallback Module::newDataModuleHandler(kj::ArrayPtr<const kj::byte> data) {
  return [data](Lock& js, const Url& specifier, const ModuleNamespace& ns,
             const CompilationObserver&) -> bool {
    return js.tryCatch([&] {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, data.size());
      backing.asArrayPtr().copyFrom(data);
      auto buffer = jsg::BufferSource(js, kj::mv(backing));
      return ns.setDefault(js, JsValue(buffer.getHandle(js)));
    }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Module::EvaluateCallback Module::newJsonModuleHandler(kj::ArrayPtr<const char> data) {
  return [data](Lock& js, const Url& specifier, const ModuleNamespace& ns,
             const CompilationObserver& observer) -> bool {
    return js.tryCatch([&] {
      auto metrics = observer.onJsonCompilationStart(js.v8Isolate, data.size());
      return ns.setDefault(js, JsValue(js.parseJson(data).getHandle(js)));
    }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Module::EvaluateCallback Module::newWasmModuleHandler(kj::ArrayPtr<const kj::byte> data) {
  struct Cache final {
    kj::MutexGuarded<kj::Maybe<v8::CompiledWasmModule>> mutex{};
  };
  return [data, cache = kj::heap<Cache>()](Lock& js, const Url& specifier,
             const ModuleNamespace& ns, const CompilationObserver& observer) mutable -> bool {
    return js.tryCatch([&]() -> bool {
      js.setAllowEval(true);
      KJ_DEFER(js.setAllowEval(false));

      // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
      // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
      // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
      // compiles fast but runs slower.
      AllowV8BackgroundThreadsScope scope;

      {
        // See if we can use a cached compiled module to speed things up.
        auto lock = cache->mutex.lockShared();
        KJ_IF_SOME(compiled, *lock) {
          auto metrics = observer.onWasmCompilationFromCacheStart(js.v8Isolate);
          auto result =
              JsValue(check(v8::WasmModuleObject::FromCompiledModule(js.v8Isolate, compiled)));
          return ns.setDefault(js, result);
        }
      }

      auto module = jsg::compileWasmModule(js, data, observer);
      auto lock = cache->mutex.lockExclusive();
      *lock = module->GetCompiledModule();
      auto result = JsValue(module);
      return ns.setDefault(js, result);
    }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Function<void()> Module::compileEvalFunction(Lock& js,
    kj::StringPtr code,
    kj::StringPtr name,
    kj::Maybe<JsObject> compileExtensions,
    const CompilationObserver& observer) {
  auto metrics = observer.onScriptCompilationStart(js.v8Isolate, name);
  v8::ScriptOrigin origin(js.str(name));
  v8::ScriptCompiler::Source source(js.str(code), origin);
  auto fn = ([&] {
    KJ_IF_SOME(ext, compileExtensions) {
      v8::Local<v8::Object> obj = ext;
      return check(
          v8::ScriptCompiler::CompileFunction(js.v8Context(), &source, 0, nullptr, 1, &obj));
    } else {
      return check(
          v8::ScriptCompiler::CompileFunction(js.v8Context(), &source, 0, nullptr, 0, nullptr));
    }
  })();

  return [ref = js.v8Ref(fn)](Lock& js) mutable {
    js.withinHandleScope([&] {
      // Any return value is explicitly ignored.
      JsValue(check(ref.getHandle(js)->Call(js.v8Context(), js.v8Context()->Global(), 0, nullptr)));
    });
  };
}

}  // namespace workerd::jsg::modules
