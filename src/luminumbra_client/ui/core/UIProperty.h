#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace Luminumbra::Client::UI {

// Forward declaration for callbacks
template<typename T>
class Property;

template<typename T>
using PropertyCallback = std::function<void(const T&, const T&)>; // (oldValue, newValue)

// Token identifying a single Subscribe registration. Pass it back to
// Unsubscribe to remove the callback. 0 is never a valid token.
using SubscriptionToken = std::uint64_t;
inline constexpr SubscriptionToken kInvalidSubscriptionToken = 0;

template<typename T>
struct IsVector : std::false_type {};

template<typename T, typename Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {};

/**
 * Observable property that can notify listeners when its value changes.
 * Core component of the reactive data binding system.
 *
 * Lifetime contract: every Subscribe MUST be paired with an Unsubscribe
 * (directly, or via ScopedSubscription / UIComponent binding cleanup) before
 * the subscriber is destroyed, otherwise a later Set invokes a dangling
 * callback (use-after-free).
 */
template<typename T>
class Property {
public:
    Property() = default;
    explicit Property(T initial_value) : m_value(std::move(initial_value)) {}

    // Get current value
    const T& Get() const { return m_value; }

    // Set value and notify observers if changed
    void Set(T new_value) {
        if (m_value != new_value) {
            T old_value = std::move(m_value);
            m_value = std::move(new_value);
            NotifyObservers(old_value, m_value);
        }
    }

    // Subscribe to value changes. Returns a token for Unsubscribe.
    SubscriptionToken Subscribe(PropertyCallback<T> callback) {
        const SubscriptionToken token = m_next_token++;
        m_subscriptions.push_back(Subscription{token, std::move(callback)});
        return token;
    }

    // Remove a previously registered callback. Safe to call with a token that
    // was already removed (returns false). Safe to call from inside a
    // notification callback: the in-flight notification iterates a snapshot,
    // so removal takes effect from the next Set.
    bool Unsubscribe(SubscriptionToken token) {
        auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                               [token](const Subscription& s) { return s.token == token; });
        if (it == m_subscriptions.end()) {
            return false;
        }
        m_subscriptions.erase(it);
        return true;
    }

    // Number of currently registered callbacks.
    std::size_t SubscriberCount() const { return m_subscriptions.size(); }

    // Operator overloads for convenience
    Property& operator=(const T& value) {
        Set(value);
        return *this;
    }

    operator const T&() const {
        return Get();
    }

    // For container properties, provide mutation methods that trigger notifications
    template<typename U = T, typename = std::enable_if_t<IsVector<U>::value>>
    void Add(const typename U::value_type& item) {
        m_value.push_back(item);
        NotifyObservers(m_value, m_value); // Same value but content changed
    }

    template<typename U = T, typename = std::enable_if_t<IsVector<U>::value>>
    void Remove(std::size_t index) {
        if (index < m_value.size()) {
            m_value.erase(m_value.begin() + index);
            NotifyObservers(m_value, m_value);
        }
    }

    template<typename U = T, typename = std::enable_if_t<IsVector<U>::value>>
    void Clear() {
        if (!m_value.empty()) {
            T old_value = m_value;
            m_value.clear();
            NotifyObservers(old_value, m_value);
        }
    }

private:
    struct Subscription {
        SubscriptionToken token = kInvalidSubscriptionToken;
        PropertyCallback<T> callback;
    };

    T m_value{};
    std::vector<Subscription> m_subscriptions;
    SubscriptionToken m_next_token = 1;

    void NotifyObservers(const T& old_value, const T& new_value) {
        // Iterate a snapshot so callbacks may Subscribe/Unsubscribe on this
        // property without invalidating the loop.
        const std::vector<Subscription> snapshot = m_subscriptions;
        for (const Subscription& subscription : snapshot) {
            if (subscription.callback) {
                subscription.callback(old_value, new_value);
            }
        }
    }
};

/**
 * RAII handle for a Property subscription: unsubscribes on destruction.
 * Move-only. The referenced Property must outlive the handle.
 */
class ScopedSubscription {
public:
    ScopedSubscription() = default;

    template<typename T>
    ScopedSubscription(Property<T>& property, SubscriptionToken token)
        : m_unsubscribe([&property, token]() { property.Unsubscribe(token); }) {}

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& other) noexcept
        : m_unsubscribe(std::move(other.m_unsubscribe)) {
        other.m_unsubscribe = nullptr;
    }

    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
        if (this != &other) {
            Reset();
            m_unsubscribe = std::move(other.m_unsubscribe);
            other.m_unsubscribe = nullptr;
        }
        return *this;
    }

    ~ScopedSubscription() { Reset(); }

    // Unsubscribe now (idempotent).
    void Reset() {
        if (m_unsubscribe) {
            m_unsubscribe();
            m_unsubscribe = nullptr;
        }
    }

    bool Active() const { return static_cast<bool>(m_unsubscribe); }

private:
    std::function<void()> m_unsubscribe;
};

} // namespace Luminumbra::Client::UI
