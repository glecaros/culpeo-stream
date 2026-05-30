namespace CulpeoStream.Client;

/// <summary>
/// Connection state of a <see cref="CulpeoStreamClient"/>.
/// </summary>
public enum CulpeoClientState
{
    /// <summary>Not connected. Initial state, or after <see cref="CulpeoStreamClient.DisconnectAsync"/> completes.</summary>
    Disconnected,

    /// <summary>WebSocket is being established and <c>culpeo.init</c> handshake is in progress.</summary>
    Connecting,

    /// <summary>Session is fully established. Media and events can flow.</summary>
    Established,

    /// <summary>Connection was lost and automatic reconnection is in progress.</summary>
    Reconnecting,
}
