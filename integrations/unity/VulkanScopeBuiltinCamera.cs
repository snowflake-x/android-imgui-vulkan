using UnityEngine;

/**
 * @brief Measures the Built-in pipeline's selected camera image before the final output blit.
 * @note Attach only to the final game camera and place before any upscaling image effect.
 * @note Hardware dynamic resolution and XR are deliberately unsupported by this adapter.
 */
[RequireComponent(typeof(Camera))]
public sealed class VulkanScopeBuiltinCamera : MonoBehaviour
{
    private Camera observedCamera;

    private void Awake()
    {
        observedCamera = GetComponent<Camera>();
    }

    private void OnRenderImage(RenderTexture source, RenderTexture destination)
    {
        if (VulkanScopeTelemetry.IsSupportedCamera(observedCamera) && source != null &&
            !source.useDynamicScale)
        {
            VulkanScopeTelemetry.Report(source.width, source.height, Screen.width, Screen.height,
                "Built-in/OnRenderImage pre-upscale");
        }
        Graphics.Blit(source, destination);
    }
}
