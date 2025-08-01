/*
 * Copyright (c) 2023 Runner-2019
 * Copyright (c) 2023 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "../stdexec/execution.hpp"
#include "../stdexec/__detail/__meta.hpp"
#include "../stdexec/__detail/__basic_sender.hpp"
#include "../stdexec/__detail/__manual_lifetime.hpp"

#include "trampoline_scheduler.hpp"
#include "sequence.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <type_traits>

namespace exec {
  namespace __repeat_n {
    using namespace stdexec;

    template <class _Sender, class _Receiver>
    struct __repeat_n_state;

    template <class _SenderId, class _ReceiverId>
    struct __receiver {
      using _Sender = stdexec::__t<_SenderId>;
      using _Receiver = stdexec::__t<_ReceiverId>;

      struct __t {
        using __id = __receiver;
        using receiver_concept = stdexec::receiver_t;
        __repeat_n_state<_Sender, _Receiver> *__state_;

        template <class... _Args>
        void set_value(_Args &&...__args) noexcept {
          __state_->__complete(set_value_t(), static_cast<_Args &&>(__args)...);
        }

        template <class _Error>
        void set_error(_Error &&__err) noexcept {
          __state_->__complete(set_error_t(), static_cast<_Error &&>(__err));
        }

        void set_stopped() noexcept {
          __state_->__complete(set_stopped_t());
        }

        auto get_env() const noexcept -> env_of_t<_Receiver> {
          return stdexec::get_env(__state_->__receiver());
        }
      };
    };

    template <class _Child>
    struct __child_count_pair {
      _Child __child_;
      std::size_t __count_;
    };

    template <class _Child>
    __child_count_pair(_Child, std::size_t) -> __child_count_pair<_Child>;

    STDEXEC_PRAGMA_PUSH()

    STDEXEC_PRAGMA_IGNORE_GNU("-Wtsan")

    template <class _Sender, class _Receiver>
    struct __repeat_n_state
      : stdexec::__enable_receiver_from_this<
          _Sender,
          _Receiver,
          __repeat_n_state<_Sender, _Receiver>
        > {
      using __child_count_pair_t = __decay_t<__data_of<_Sender>>;
      using __child_t = decltype(__child_count_pair_t::__child_);
      using __receiver_t = stdexec::__t<__receiver<__id<_Sender>, __id<_Receiver>>>;
      using __child_on_sched_sender_t =
        __result_of<exec::sequence, schedule_result_t<trampoline_scheduler&>, __child_t &>;
      using __child_op_t = stdexec::connect_result_t<__child_on_sched_sender_t, __receiver_t>;

      __child_count_pair<__child_t> __pair_;
      std::atomic_flag __started_{};
      stdexec::__manual_lifetime<__child_op_t> __child_op_;
      trampoline_scheduler __sched_;

      __repeat_n_state(_Sender &&__sndr, _Receiver &)
        : __pair_(__sexpr_apply(static_cast<_Sender &&>(__sndr), stdexec::__detail::__get_data())) {
        // Q: should we skip __connect() if __count_ == 0?
        __connect();
      }

      ~__repeat_n_state() {
        if (!__started_.test(std::memory_order_acquire)) {
          std::atomic_thread_fence(std::memory_order_release);
          // TSan does not support std::atomic_thread_fence, so we
          // need to use the TSan-specific __tsan_release instead:
          STDEXEC_TSAN(__tsan_release(&__started_));
          __child_op_.__destroy();
        }
      }

      void __connect() {
        __child_op_.__construct_from([this] {
          return stdexec::connect(
            exec::sequence(stdexec::schedule(__sched_), __pair_.__child_), __receiver_t{this});
        });
      }

      void __start() noexcept {
        if (__pair_.__count_ == 0) {
          stdexec::set_value(static_cast<_Receiver &&>(this->__receiver()));
        } else {
          [[maybe_unused]]
          const bool __already_started = __started_.test_and_set(std::memory_order_relaxed);
          STDEXEC_ASSERT(!__already_started);
          stdexec::start(__child_op_.__get());
        }
      }

      template <class _Tag, class... _Args>
      void __complete(_Tag, _Args... __args) noexcept { // Intentionally by value...
        STDEXEC_ASSERT(__pair_.__count_ > 0);
        __child_op_.__destroy(); // ... because this could potentially invalidate them.
        if constexpr (same_as<_Tag, set_value_t>) {
          STDEXEC_TRY {
            if (--__pair_.__count_ == 0) {
              stdexec::set_value(static_cast<_Receiver &&>(this->__receiver()));
            } else {
              __connect();
              stdexec::start(__child_op_.__get());
            }
          }
          STDEXEC_CATCH_ALL {
            stdexec::set_error(
              static_cast<_Receiver &&>(this->__receiver()), std::current_exception());
          }
        } else {
          _Tag()(static_cast<_Receiver &&>(this->__receiver()), static_cast<_Args &&>(__args)...);
        }
      }
    };

    STDEXEC_PRAGMA_POP()

    struct repeat_n_t;
    struct _REPEAT_N_EXPECTS_A_SENDER_OF_VOID_;

    template <class _Sender, class... _Args>
    using __values_t =
      // There's something funny going on with __if_c here. Use std::conditional_t instead. :-(
      std::conditional_t<
        (sizeof...(_Args) == 0),
        completion_signatures<>,
        __mexception<
          _REPEAT_N_EXPECTS_A_SENDER_OF_VOID_,
          _WHERE_(_IN_ALGORITHM_, repeat_n_t),
          _WITH_SENDER_<_Sender>
        >
      >;

    template <class _Error>
    using __error_t = completion_signatures<set_error_t(__decay_t<_Error>)>;

    template <class _Pair, class... _Env>
    using __completions_t = stdexec::transform_completion_signatures<
      __completion_signatures_of_t<decltype(__decay_t<_Pair>::__child_) &, _Env...>,
      stdexec::transform_completion_signatures<
        __completion_signatures_of_t<stdexec::schedule_result_t<exec::trampoline_scheduler>, _Env...>,
        __eptr_completion,
        __sigs::__default_set_value,
        __error_t
      >,
      __mbind_front_q<__values_t, decltype(__decay_t<_Pair>::__child_)>::template __f,
      __error_t
    >;

    struct __repeat_n_tag { };

    struct __repeat_n_impl : __sexpr_defaults {
      static constexpr auto get_completion_signatures =
        []<class _Sender, class... _Env>(_Sender &&, _Env &&...) noexcept {
          return __completions_t<__data_of<_Sender>, _Env...>{};
        };

      static constexpr auto get_state =
        []<class _Sender, class _Receiver>(_Sender &&__sndr, _Receiver &__rcvr) {
          return __repeat_n_state{static_cast<_Sender &&>(__sndr), __rcvr};
        };

      static constexpr auto start = [](auto &__state, __ignore) noexcept -> void {
        __state.__start();
      };
    };

    struct repeat_n_t {
      template <sender _Sender>
      auto operator()(_Sender &&__sndr, std::size_t __count) const {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain, __make_sexpr<repeat_n_t>(__count, static_cast<_Sender &&>(__sndr)));
      }

      STDEXEC_ATTRIBUTE(always_inline)

      constexpr auto
        operator()(std::size_t __count) const -> __binder_back<repeat_n_t, std::size_t> {
        return {{__count}, {}, {}};
      }

      template <class _Sender>
      auto transform_sender(_Sender &&__sndr, __ignore) {
        return __sexpr_apply(
          static_cast<_Sender &&>(__sndr),
          []<class _Child>(__ignore, std::size_t __count, _Child __child) {
            return __make_sexpr<__repeat_n_tag>(__child_count_pair{std::move(__child), __count});
          });
      }
    };
  } // namespace __repeat_n

  using __repeat_n::repeat_n_t;
  inline constexpr repeat_n_t repeat_n{};
} // namespace exec

namespace stdexec {
  template <>
  struct __sexpr_impl<exec::__repeat_n::__repeat_n_tag> : exec::__repeat_n::__repeat_n_impl { };

  template <>
  struct __sexpr_impl<exec::repeat_n_t> : __sexpr_defaults {
    static constexpr auto get_completion_signatures = []<class _Sender>(_Sender &&) noexcept
      -> __completion_signatures_of_t<transform_sender_result_t<default_domain, _Sender, env<>>> {
    };
  };
} // namespace stdexec
