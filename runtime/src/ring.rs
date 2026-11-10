//! Bounded single-producer/single-consumer queue. Roles never migrate
//! concurrently. Values are Copy; no allocation occurs after construction.
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicUsize, Ordering};

pub struct Ring<T: Copy, const N: usize> {
    slots: Box<[UnsafeCell<T>]>,
    read: AtomicUsize,
    write: AtomicUsize,
}
unsafe impl<T: Copy + Send, const N: usize> Sync for Ring<T, N> {}
unsafe impl<T: Copy + Send, const N: usize> Send for Ring<T, N> {}
impl<T: Copy + Default, const N: usize> Ring<T, N> {
    pub fn new() -> Self {
        assert!(N > 0);
        // Allocate in-place on the heap. A large temporary fixed array here
        // would overflow typical Rack worker / Rust test-thread stacks.
        let slots = (0..N)
            .map(|_| UnsafeCell::new(T::default()))
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            slots,
            read: AtomicUsize::new(0),
            write: AtomicUsize::new(0),
        }
    }
    /// Only the producer may call push.
    pub fn push(&self, value: T) -> bool {
        let w = self.write.load(Ordering::Relaxed);
        let r = self.read.load(Ordering::Acquire);
        if w.wrapping_sub(r) >= N {
            return false;
        }
        unsafe {
            *self.slots[w % N].get() = value;
        }
        self.write.store(w.wrapping_add(1), Ordering::Release);
        true
    }
    /// Only the consumer may call pop/peek.
    pub fn pop(&self) -> Option<T> {
        let r = self.read.load(Ordering::Relaxed);
        if r == self.write.load(Ordering::Acquire) {
            return None;
        }
        let v = unsafe { *self.slots[r % N].get() };
        self.read.store(r.wrapping_add(1), Ordering::Release);
        Some(v)
    }
    pub fn peek(&self) -> Option<T> {
        let r = self.read.load(Ordering::Relaxed);
        if r == self.write.load(Ordering::Acquire) {
            None
        } else {
            Some(unsafe { *self.slots[r % N].get() })
        }
    }
    pub fn len(&self) -> usize {
        self.write
            .load(Ordering::Acquire)
            .wrapping_sub(self.read.load(Ordering::Acquire))
            .min(N)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    #[test]
    fn fifo_full_empty_wrap() {
        let q = Ring::<u32, 4>::new();
        assert_eq!(q.pop(), None);
        for round in 0..20 {
            for n in 0..4 {
                assert!(q.push(round * 4 + n));
            }
            assert!(!q.push(99));
            for n in 0..4 {
                assert_eq!(q.peek(), Some(round * 4 + n));
                assert_eq!(q.pop(), Some(round * 4 + n));
            }
        }
    }
    #[test]
    fn concurrent_fifo() {
        let q = Arc::new(Ring::<u32, 256>::new());
        let p = q.clone();
        let writer = std::thread::spawn(move || {
            for n in 0..50_000 {
                while !p.push(n) {
                    std::thread::yield_now();
                }
            }
        });
        for n in 0..50_000 {
            loop {
                if let Some(v) = q.pop() {
                    assert_eq!(v, n);
                    break;
                }
                std::thread::yield_now();
            }
        }
        writer.join().unwrap();
    }
}
