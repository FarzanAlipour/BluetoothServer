using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using BluetoothServer.Unity;
using NUnit.Framework;
using UnityEngine;

namespace BleSample.Tests
{
    // Platform-agnostic EditMode tests: they run on whichever OS hosts the Editor. They never touch
    // real Bluetooth (no Initialize, no advertising), so they need no Bluetooth permission.
    public class BleSampleTests
    {
        private static string PluginsDir => Path.Combine(Application.dataPath, "Plugins");

        // Canonical, committed import settings. Unity deletes a .meta whose binary is missing (the
        // binaries are not committed), so build_plugin.* / run_tests.* install the host OS's copy
        // next to the binary. See README "Why PluginMeta/".
        private static string MetaDir => Path.GetFullPath(Path.Combine(Application.dataPath, "..", "PluginMeta"));

        // OS -> (relative plugin path, UnityEditor/Standalone platform name that must be enabled).
        private static readonly (string File, string Os, string Standalone)[] Plugins =
        {
            ("macOS/libBleServer.dylib", "OSX", "OSXUniversal"),
            ("Windows/x86_64/BleServer.dll", "Windows", "Win64"),
            ("Linux/x86_64/libBleServer.so", "Linux", "Linux64"),
        };

        private static string HostPluginFile()
        {
            switch (Application.platform)
            {
                case RuntimePlatform.OSXEditor: return Plugins[0].File;
                case RuntimePlatform.WindowsEditor: return Plugins[1].File;
                case RuntimePlatform.LinuxEditor: return Plugins[2].File;
                default: return null;
            }
        }

        [Test]
        public void ProjectCompiles_SampleAndWrapperTypesResolve()
        {
            Assert.IsNotNull(typeof(BleSampleController));
            Assert.IsNotNull(typeof(BleServer));
            Assert.IsNotNull(typeof(BleChunker));
        }

        [Test]
        public void NativePlugin_ForHostOs_ExistsOnDisk()
        {
            string file = HostPluginFile();
            Assert.IsNotNull(file, "Unsupported Editor platform: " + Application.platform);
            string path = Path.Combine(PluginsDir, file);
            Assert.IsTrue(File.Exists(path), "Native plugin missing: " + path + ". Run build_plugin.sh / build_plugin.ps1.");
            Assert.Greater(new FileInfo(path).Length, 1024, "Native plugin is suspiciously small: " + path);
        }

        [Test]
        public void NativePlugin_ForHostOs_LoadsAndAnswersPInvoke()
        {
            // P/Invoke smoke test: ABI version + build info, then create/destroy a server handle.
            // Does not call Initialize, so CoreBluetooth/BlueZ/WinRT are never touched.
            Assert.Greater(BleServer.NativeApiVersion, 0);
            using (var server = new BleServer())
            {
                Assert.Greater(server.ApiVersion, 0);
                StringAssert.IsMatch("\\S", server.BuildInfo);
                Debug.Log("Native plugin: API v" + server.ApiVersion + " " + server.BuildInfo);
            }
        }

        [Test]
        public void GattLayout_UuidsAreValidDistinct128BitUuids()
        {
            var all = new[] { BleSampleGatt.ServiceUuid }.Concat(BleSampleGatt.CharacteristicUuids).ToArray();
            foreach (string uuid in all)
            {
                Assert.IsTrue(Guid.TryParse(uuid, out _), "Not a UUID: " + uuid);
                Assert.AreEqual(36, uuid.Length, "Expected the 36-char 128-bit form: " + uuid);
                Assert.AreEqual(uuid.ToLowerInvariant(), uuid, "Use lowercase UUIDs: " + uuid);
            }
            Assert.AreEqual(all.Length, all.Distinct().Count(), "Duplicate UUID in the GATT layout");
            Assert.AreEqual(5, BleSampleGatt.CharacteristicUuids.Length);
        }

        [Test]
        public void GattLayout_AdvertisedNameFitsAdvertisingPacket()
        {
            // 31-byte legacy advertising payload; a complete-name AD structure costs 2 bytes of overhead.
            Assert.LessOrEqual(Encoding.UTF8.GetByteCount(BleSampleGatt.AdvertisedName), 29);
            Assert.IsNotEmpty(BleSampleGatt.AdvertisedName);
        }

        [Test]
        public void PluginMetaFiles_ExistForAllThreeOses_AndRestrictToOwnOs()
        {
            foreach (var plugin in Plugins)
            {
                string metaPath = Path.Combine(MetaDir, plugin.File + ".meta");
                Assert.IsTrue(File.Exists(metaPath), "Missing committed .meta: " + metaPath);
                string meta = File.ReadAllText(metaPath);

                Assert.IsTrue(Enabled(meta, "Any: ") == 0, plugin.File + ": 'Any' platform must be disabled");
                Assert.AreEqual(1, Enabled(meta, "Editor: Editor"), plugin.File + ": Editor must be enabled");
                StringAssert.Contains("OS: " + plugin.Os, EditorSettings(meta), plugin.File + ": Editor OS must be " + plugin.Os);
                Assert.AreEqual(1, Enabled(meta, "Standalone: " + plugin.Standalone), plugin.File + ": Standalone " + plugin.Standalone + " must be enabled");

                // Every other desktop standalone target, and all remaining platform entries, must be off.
                foreach (Match m in Regex.Matches(meta, @"first:\s*\n\s*(\w+): ?(\w*)\s*\n\s*second:\s*\n\s*enabled: (\d)"))
                {
                    string key = m.Groups[1].Value + ": " + m.Groups[2].Value;
                    bool own = key.StartsWith("Editor: Editor") || key == "Standalone: " + plugin.Standalone;
                    if (!own)
                    {
                        Assert.AreEqual("0", m.Groups[3].Value, plugin.File + ": platform '" + key + "' must be disabled");
                    }
                }
            }
        }

        [Test]
        public void PluginMeta_InstalledNextToHostBinary_MatchesCommittedCopy()
        {
            string file = HostPluginFile();
            Assert.IsNotNull(file);
            string installed = Path.Combine(PluginsDir, file + ".meta");
            Assert.IsTrue(File.Exists(installed), "Import settings not installed: " + installed + " (run build_plugin.* or run_tests.*)");
            Assert.AreEqual(File.ReadAllText(Path.Combine(MetaDir, file + ".meta")), File.ReadAllText(installed),
                "Installed .meta differs from PluginMeta/ (Unity changed the import settings?)");
        }

        [Test]
        public void PostBuild_AddsBluetoothUsageDescription_Idempotently()
        {
            string path = Path.Combine(Path.GetTempPath(), "BleSampleInfo-" + Guid.NewGuid().ToString("N") + ".plist");
            File.WriteAllText(path, "<?xml version=\"1.0\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>CFBundleName</key>\n\t<string>X</string>\n</dict>\n</plist>\n");
            try
            {
                Assert.IsTrue(BleSample.Editor.BleSamplePostBuild.EnsureBluetoothUsageDescription(path));
                Assert.IsFalse(BleSample.Editor.BleSamplePostBuild.EnsureBluetoothUsageDescription(path));
                string text = File.ReadAllText(path);
                StringAssert.Contains("<key>NSBluetoothAlwaysUsageDescription</key>", text);
                Assert.AreEqual(1, Regex.Matches(text, "NSBluetoothAlwaysUsageDescription").Count);
            }
            finally
            {
                File.Delete(path);
            }
        }

        [Test]
        public void ReadAndWriteHelpers_FormatBytes()
        {
            Assert.AreEqual("48 65 6C 6C 6F", BleSampleController.ToHex(Encoding.UTF8.GetBytes("Hello")));
            Assert.AreEqual("<empty>", BleSampleController.ToHex(new byte[0]));
            Assert.AreEqual("Hello", BleSampleController.ToUtf8(Encoding.UTF8.GetBytes("Hello")));
        }

        // ---- .meta helpers ----

        private static int Enabled(string meta, string firstLine)
        {
            var m = Regex.Match(meta, @"first:\s*\n\s*" + Regex.Escape(firstLine.TrimEnd()) + @" ?\s*\n\s*second:\s*\n\s*enabled: (\d)");
            return m.Success ? int.Parse(m.Groups[1].Value) : -1;
        }

        private static string EditorSettings(string meta)
        {
            int i = meta.IndexOf("Editor: Editor", StringComparison.Ordinal);
            return i < 0 ? string.Empty : meta.Substring(i, Math.Min(400, meta.Length - i));
        }
    }
}
