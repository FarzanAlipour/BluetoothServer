using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace BleSample.Editor
{
    // Regenerates Assets/Scenes/BleSampleScene.unity (also callable in batchmode:
    //   -executeMethod BleSample.Editor.BleSampleSceneBuilder.Build).
    public static class BleSampleSceneBuilder
    {
        public const string ScenePath = "Assets/Scenes/BleSampleScene.unity";

        [MenuItem("BleSample/Rebuild sample scene")]
        public static void Build()
        {
            Directory.CreateDirectory("Assets/Scenes");
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var cameraGo = new GameObject("Main Camera") { tag = "MainCamera" };
            var camera = cameraGo.AddComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(0.12f, 0.13f, 0.16f);

            new GameObject("BleSampleController").AddComponent<BleSampleController>();

            EditorSceneManager.SaveScene(scene, ScenePath);
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
            AssetDatabase.SaveAssets();
        }
    }
}
