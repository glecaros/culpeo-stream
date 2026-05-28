using System.Collections.Concurrent;

namespace CulpeoStream.AspNetCore.Internal;

/// <summary>
/// Per-IP sliding-window connection rate limiter.
///
/// Uses a 60-second sliding window. Old entries are pruned lazily on each
/// <see cref="TryAcquire"/> call so the dictionary does not grow without bound.
/// </summary>
internal sealed class IpRateLimiter
{
    private readonly ConcurrentDictionary<string, IpRecord> _records = new(StringComparer.Ordinal);

    /// <summary>
    /// Returns <see langword="true"/> if the IP is within its rate limit and
    /// records the attempt. Returns <see langword="false"/> if the limit is
    /// exceeded; in that case the attempt is NOT counted.
    /// </summary>
    public bool TryAcquire(string ip, int maxPerMinute)
    {
        if (maxPerMinute <= 0)
        {
            return true; // rate limiting disabled
        }

        var record = _records.GetOrAdd(ip, _ => new IpRecord());
        return record.TryAcquire(maxPerMinute);
    }

    private sealed class IpRecord
    {
        private readonly Queue<long> _timestamps = new(); // ticks
        private readonly object _sync = new();

        public bool TryAcquire(int max)
        {
            lock (_sync)
            {
                var nowTicks = DateTimeOffset.UtcNow.Ticks;
                var cutoffTicks = nowTicks - TimeSpan.FromMinutes(1).Ticks;

                // Evict expired entries
                while (_timestamps.Count > 0 && _timestamps.Peek() < cutoffTicks)
                {
                    _timestamps.Dequeue();
                }

                if (_timestamps.Count >= max)
                {
                    return false;
                }

                _timestamps.Enqueue(nowTicks);
                return true;
            }
        }
    }
}
