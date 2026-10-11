using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

/**
 * @brief Captures URP 14 camera-color viewport pixels before post-processing/upscaling.
 * @note Requires Unity 2022.3 + URP 14. Excludes XR and hardware dynamic resolution.
 * @note Add to the Universal Renderer used by the one selected base game camera.
 */
public sealed class VulkanScopeUrp14Feature : ScriptableRendererFeature
{
    [SerializeField] private string cameraTag = "MainCamera";
    private MeasurementPass pass;

    private sealed class MeasurementPass : ScriptableRenderPass
    {
        private RTHandle color;

        public void SetTarget(RTHandle target)
        {
            color = target;
        }

        public override void Execute(ScriptableRenderContext context, ref RenderingData renderingData)
        {
            if (color == null || color.rt == null || color.rt.useDynamicScale ||
                renderingData.cameraData.cameraTargetDescriptor.useDynamicScale) return;
            Vector2Int size = color.useScaling
                ? color.GetScaledSize(color.rtHandleProperties.currentViewportSize)
                : new Vector2Int(color.rt.width, color.rt.height);
            VulkanScopeTelemetry.Report(size.x, size.y, Screen.width, Screen.height,
                "URP14/cameraColorTarget viewport pre-upscale");
        }
    }

    public override void Create()
    {
        pass = new MeasurementPass { renderPassEvent = RenderPassEvent.BeforeRenderingPostProcessing };
    }

    private bool Observe(in RenderingData renderingData)
    {
        return renderingData.cameraData.renderType == CameraRenderType.Base &&
            VulkanScopeTelemetry.IsSupportedCamera(renderingData.cameraData.camera) &&
            renderingData.cameraData.camera.CompareTag(cameraTag);
    }

    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
    {
        if (Observe(in renderingData)) renderer.EnqueuePass(pass);
    }

    public override void SetupRenderPasses(ScriptableRenderer renderer, in RenderingData renderingData)
    {
        if (Observe(in renderingData)) pass.SetTarget(renderer.cameraColorTargetHandle);
    }
}
