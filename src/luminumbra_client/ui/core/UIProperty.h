#pragma once

#include <functional>
#include <vector>
#include <memory>

namespace Luminumbra::Client::UI {

// Forward declaration for callbacks
template<typename T>
class Property;

template<typename T>
using PropertyCallback = std::function<void(const T&, const T&)>; // (oldValue, newValue)

/**
 * Observable property that can notify listeners when its value changes.
 * Core component of the reactive data binding system.
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
    
    // Subscribe to value changes
    void Subscribe(PropertyCallback<T> callback) {
        m_callbacks.emplace_back(std::move(callback));
    }
    
    // Operator overloads for convenience
    Property& operator=(const T& value) {
        Set(value);
        return *this;
    }
    
    operator const T&() const {
        return Get();
    }
    
    // For container properties, provide mutation methods that trigger notifications
    template<typename U = T>
    typename std::enable_if_t<std::is_same_v<U, std::vector<typename U::value_type>>, void>
    Add(const typename T::value_type& item) {
        m_value.push_back(item);
        NotifyObservers(m_value, m_value); // Same value but content changed
    }
    
    template<typename U = T>
    typename std::enable_if_t<std::is_same_v<U, std::vector<typename U::value_type>>, void>
    Remove(size_t index) {
        if (index < m_value.size()) {
            m_value.erase(m_value.begin() + index);
            NotifyObservers(m_value, m_value);
        }
    }
    
    template<typename U = T>
    typename std::enable_if_t<std::is_same_v<U, std::vector<typename U::value_type>>, void>
    Clear() {
        if (!m_value.empty()) {
            T old_value = m_value;
            m_value.clear();
            NotifyObservers(old_value, m_value);
        }
    }

private:
    T m_value{};
    std::vector<PropertyCallback<T>> m_callbacks;
    
    void NotifyObservers(const T& old_value, const T& new_value) {
        for (auto& callback : m_callbacks) {
            if (callback) {
                callback(old_value, new_value);
            }
        }
    }
};

} // namespace Luminumbra::Client::UI