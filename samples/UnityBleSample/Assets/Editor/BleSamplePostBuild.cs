using System.IO;
using UnityEditor;
using UnityEditor.Callbacks;

namespace BleSample.Editor
{
    // macOS standalone players need NSBluetoothAlwaysUsageDescription in Info.plist, otherwise the OS
    // kills the app on the first CoreBluetooth call. Windows and Linux players need nothing here.
    public static class BleSamplePostBuild
    {
        public const string Key = "NSBluetoothAlwaysUsageDescription";
        public const string Description = "BoostanBLE Sample acts as a Bluetooth LE peripheral (GATT server).";

        [PostProcessBuild(100)]
        public static void OnPostProcessBuild(BuildTarget target, string pathToBuiltProject)
        {
            if (target != BuildTarget.StandaloneOSX)
            {
                return;
            }
            string plist = Path.Combine(pathToBuiltProject, "Contents", "Info.plist");
            if (File.Exists(plist))
            {
                EnsureBluetoothUsageDescription(plist);
            }
        }

        // Returns true when the file was modified. Idempotent.
        public static bool EnsureBluetoothUsageDescription(string plistPath)
        {
            string text = File.ReadAllText(plistPath);
            if (text.Contains("<key>" + Key + "</key>"))
            {
                return false;
            }
            int end = text.LastIndexOf("</dict>", System.StringComparison.Ordinal);
            if (end < 0)
            {
                throw new InvalidDataException("Info.plist has no </dict>: " + plistPath);
            }
            string entry = "\t<key>" + Key + "</key>\n\t<string>" + Description + "</string>\n";
            File.WriteAllText(plistPath, text.Insert(end, entry));
            return true;
        }
    }
}
