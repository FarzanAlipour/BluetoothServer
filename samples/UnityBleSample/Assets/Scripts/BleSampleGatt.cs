namespace BleSample
{
    // GATT layout of the sample. Documented in samples/UnityBleSample/README.md; keep both in sync.
    // All UUIDs are custom 128-bit UUIDs (not Bluetooth SIG assigned numbers).
    public static class BleSampleGatt
    {
        public const string AdvertisedName = "BoostanBLE-Sample";

        public const string ServiceUuid = "7b1e0000-5a6c-4f0e-9d3b-2c1a8e4f6b01";

        // Read: "uptime=<s>s read#<n>". The provider runs on a native thread.
        public const string ReadUuid = "7b1e0001-5a6c-4f0e-9d3b-2c1a8e4f6b01";
        // Write: bytes are logged as hex + UTF-8.
        public const string WriteUuid = "7b1e0002-5a6c-4f0e-9d3b-2c1a8e4f6b01";
        // Notify: heartbeat every second, plus the "Send notify" button.
        public const string NotifyUuid = "7b1e0003-5a6c-4f0e-9d3b-2c1a8e4f6b01";
        // Echo (write + notify): whatever is written comes back as a notification.
        public const string EchoUuid = "7b1e0004-5a6c-4f0e-9d3b-2c1a8e4f6b01";
        // Large message (notify + write): BleMessageChunker chunks (256 B each) go out as notifications;
        // an ACK written back by the client is fed into the chunker.
        public const string LargeUuid = "7b1e0005-5a6c-4f0e-9d3b-2c1a8e4f6b01";

        public static readonly string[] CharacteristicUuids =
        {
            ReadUuid, WriteUuid, NotifyUuid, EchoUuid, LargeUuid
        };
    }
}
