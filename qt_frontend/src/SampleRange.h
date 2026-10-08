#pragma once

#include <QVector>
#include <algorithm>
#include <cstddef>
#include <iterator>

// Read-only view of one sample family as a single time-ordered sequence that
// may span several vectors: in a live session each lap block owns its own
// samples, and the session-wide history is those blocks back to back. It
// offers the read side of QVector that history readers use: random-access
// iterators (std::lower_bound works), indexing, size, first() and last().
//
// The view points into the vectors it was built from. Use it at once, on the
// thread that owns them; any append to those vectors invalidates it.
template <typename T>
class SampleRange {
public:
    SampleRange() = default;
    // One vector: the whole range. Implicit, so a lap's own vector can stand
    // in wherever a range is expected.
    SampleRange(const QVector<T>& rows) { append(rows); }

    void append(const QVector<T>& rows) {
        if (rows.isEmpty()) return;
        parts_.push_back({rows.constData(), size_});
        size_ += rows.size();
    }

    qsizetype size() const { return size_; }
    bool isEmpty() const { return size_ == 0; }
    const T& operator[](qsizetype i) const {
        const Part& part = partAt(i);
        return part.data[i - part.start];
    }
    const T& first() const { return parts_.front().data[0]; }
    const T& last() const { return (*this)[size_ - 1]; }

    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;
        const_iterator(const SampleRange* range, qsizetype index) : range_(range), index_(index) {}

        reference operator*() const { return element(); }
        pointer operator->() const { return &element(); }
        reference operator[](difference_type n) const { return (*range_)[index_ + n]; }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { const_iterator old = *this; ++index_; return old; }
        const_iterator& operator--() { --index_; return *this; }
        const_iterator operator--(int) { const_iterator old = *this; --index_; return old; }
        const_iterator& operator+=(difference_type n) { index_ += n; return *this; }
        const_iterator& operator-=(difference_type n) { index_ -= n; return *this; }
        friend const_iterator operator+(const_iterator it, difference_type n) { return it += n; }
        friend const_iterator operator+(difference_type n, const_iterator it) { return it += n; }
        friend const_iterator operator-(const_iterator it, difference_type n) { return it -= n; }
        friend difference_type operator-(const const_iterator& a, const const_iterator& b) {
            return difference_type(a.index_ - b.index_);
        }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.index_ == b.index_; }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) { return a.index_ != b.index_; }
        friend bool operator<(const const_iterator& a, const const_iterator& b) { return a.index_ < b.index_; }
        friend bool operator>(const const_iterator& a, const const_iterator& b) { return a.index_ > b.index_; }
        friend bool operator<=(const const_iterator& a, const const_iterator& b) { return a.index_ <= b.index_; }
        friend bool operator>=(const const_iterator& a, const const_iterator& b) { return a.index_ >= b.index_; }

    private:
        // Sequential walks stay inside one part, so the part found last is
        // checked before searching.
        const T& element() const {
            if (!part_ || index_ < part_->start || index_ >= part_->start + partSize(part_))
                part_ = &range_->partAt(index_);
            return part_->data[index_ - part_->start];
        }
        qsizetype partSize(const typename SampleRange::Part* part) const {
            const auto next = part + 1;
            return (next != range_->parts_.data() + range_->parts_.size() ? next->start : range_->size_) - part->start;
        }

        const SampleRange* range_ = nullptr;
        qsizetype index_ = 0;
        mutable const typename SampleRange::Part* part_ = nullptr;
    };

    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size_}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

private:
    struct Part { const T* data; qsizetype start; };

    const Part& partAt(qsizetype i) const {
        // The last part holding an index <= i.
        auto it = std::upper_bound(parts_.begin(), parts_.end(), i,
            [](qsizetype index, const Part& part) { return index < part.start; });
        return *(it - 1);
    }

    QVector<Part> parts_;
    qsizetype size_ = 0;
};
