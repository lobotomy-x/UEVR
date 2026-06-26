using System.Runtime.InteropServices;
using System.Numerics;

namespace UEVRPlugin {
    public static class Main {
        // This attribute makes the function look like a standard C export
        [UnmanagedCallersOnly(EntryPoint = "uevr_plugin_initialize")]
        public static bool Initialize(IntPtr param) {
            // Your C# logic here
            return true;
        }

        [UnmanagedCallersOnly(EntryPoint = "on_frame")]
        public static void OnFrame() {
            // Call the cimgui exports from UEVR.dll
            if (ImGuiNative.igBegin("C# Menu", IntPtr.Zero, 0)) {
                ImGuiNative.igText("Hello from Managed Code!");
                ImGuiNative.igEnd();
            }
        }
    }

    internal static class ImGuiNative {
        [DllImport("UEVR.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern bool igBegin(string name, IntPtr p_open, int flags);

        [DllImport("UEVR.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void igText(string fmt);

        [DllImport("UEVR.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void igEnd();
    }
}
