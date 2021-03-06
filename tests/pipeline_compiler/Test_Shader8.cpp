// Copyright (c) 2018-2020,  Zhirnov Andrey. For more information see 'LICENSE'
// ray tracing shaders was copied from glslang tests

#include "Utils.h"


extern void Test_Shader8 (VPipelineCompiler* compiler)
{
	RayTracingPipelineDesc	ppln;
	
	ppln.AddShader( RTShaderID("main"), EShader::RayGen, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
layout(binding = 0, set = 0) uniform accelerationStructureNV accNV;
layout(location = 0) rayPayloadNV vec4 payload;

void main()
{
	uint lx = gl_LaunchIDNV.x;
	uint ly = gl_LaunchIDNV.y;
	uint sx = gl_LaunchSizeNV.x;
	uint sy = gl_LaunchSizeNV.y;
	traceNV(accNV, lx, ly, sx, sy, 0u, vec3(0.0f), 0.5f, vec3(1.0f), 0.75f, 1);
}
)#" );

	ppln.AddShader( RTShaderID("PrimaryAnyHit"), EShader::RayAnyHit, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
layout(location = 1) rayPayloadInNV vec4 incomingPayload;
void main()
{
	uvec3 v0 = gl_LaunchIDNV;
	uvec3 v1 = gl_LaunchSizeNV;
	int v2 = gl_PrimitiveID;
	int v3 = gl_InstanceID;
	int v4 = gl_InstanceCustomIndexNV;
	vec3 v5 = gl_WorldRayOriginNV;
	vec3 v6 = gl_WorldRayDirectionNV;
	vec3 v7 = gl_ObjectRayOriginNV;
	vec3 v8 = gl_ObjectRayDirectionNV;
	float v9 = gl_RayTminNV;
	float v10 = gl_RayTmaxNV;
	float v11 = gl_HitTNV;
	uint v12 = gl_HitKindNV;
	mat4x3 v13 = gl_ObjectToWorldNV;
	mat4x3 v14 = gl_WorldToObjectNV;
	incomingPayload = vec4(0.5f);
	if (v2 == 1)
		ignoreIntersectionNV();
	else
		terminateRayNV();
}
)#" );

	ppln.AddShader( RTShaderID("Callable"), EShader::RayCallable, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
layout(location = 0) callableDataNV vec4 data0;
layout(location = 1) callableDataInNV dataBlock {
	uint data1;
};
void main()
{
	uvec3 id = gl_LaunchIDNV;
	uvec3 size = gl_LaunchSizeNV;
	data1 = 256U;
	executeCallableNV(2,1);
}
)#" );

	ppln.AddShader( RTShaderID("PrimaryClosestHit"), EShader::RayClosestHit, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
layout(binding = 0, set = 0) uniform accelerationStructureNV accNV;
layout(location = 0) rayPayloadNV vec4 localPayload;
layout(location = 1) rayPayloadInNV vec4 incomingPayload;
void main()
{
	uvec3 v0 = gl_LaunchIDNV;
	uvec3 v1 = gl_LaunchSizeNV;
	int v2 = gl_PrimitiveID;
	int v3 = gl_InstanceID;
	int v4 = gl_InstanceCustomIndexNV;
	vec3 v5 = gl_WorldRayOriginNV;
	vec3 v6 = gl_WorldRayDirectionNV;
	vec3 v7 = gl_ObjectRayOriginNV;
	vec3 v8 = gl_ObjectRayDirectionNV;
	float v9 = gl_RayTminNV;
	float v10 = gl_RayTmaxNV;
	float v11 = gl_HitTNV;
	uint v12 = gl_HitKindNV;
	mat4x3 v13 = gl_ObjectToWorldNV;
	mat4x3 v14 = gl_WorldToObjectNV;
	traceNV(accNV, 0u, 1u, 2u, 3u, 0u, vec3(0.5f), 0.5f, vec3(1.0f), 0.75f, 1);
}
)#" );

	ppln.AddShader( RTShaderID("PrimaryMiss"), EShader::RayMiss, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
layout(binding = 0, set = 0) uniform accelerationStructureNV accNV;
layout(location = 0) rayPayloadNV vec4 localPayload;
layout(location = 1) rayPayloadInNV vec4 incomingPayload;
void main()
{
	uvec3 v0 = gl_LaunchIDNV;
	uvec3 v1 = gl_LaunchSizeNV;
	vec3 v2 = gl_WorldRayOriginNV;
	vec3 v3 = gl_WorldRayDirectionNV;
	vec3 v4 = gl_ObjectRayOriginNV;
	vec3 v5 = gl_ObjectRayDirectionNV;
	float v6 = gl_RayTminNV;
	float v7 = gl_RayTmaxNV;
	traceNV(accNV, 0u, 1u, 2u, 3u, 0u, vec3(0.5f), 0.5f, vec3(1.0f), 0.75f, 1);
}
)#" );

	ppln.AddShader( RTShaderID("Intersection"), EShader::RayIntersection, EShaderLangFormat::VKSL_110, "main", R"#(
#version 460
#extension GL_NV_ray_tracing : enable
hitAttributeNV vec4 iAttr;
void main()
{
	uvec3 v0 = gl_LaunchIDNV;
	uvec3 v1 = gl_LaunchSizeNV;
	int v2 = gl_PrimitiveID;
	int v3 = gl_InstanceID;
	int v4 = gl_InstanceCustomIndexNV;
	vec3 v5 = gl_WorldRayOriginNV;
	vec3 v6 = gl_WorldRayDirectionNV;
	vec3 v7 = gl_ObjectRayOriginNV;
	vec3 v8 = gl_ObjectRayDirectionNV;
	float v9 = gl_RayTminNV;
	float v10 = gl_RayTmaxNV;
	mat4x3 v11 = gl_ObjectToWorldNV;
	mat4x3 v12 = gl_WorldToObjectNV;
	iAttr = vec4(0.5f,0.5f,0.0f,1.0f);
	reportIntersectionNV(0.5, 1U);
}
)#" );


	TEST( compiler->Compile( INOUT ppln, EShaderLangFormat::SPIRV_110 ));
	
	auto ds = FindDescriptorSet( ppln, DescriptorSetID("0") );
	TEST( ds );

	TEST( TestRayTracingScene( *ds, UniformID("accNV"), 0, EShaderStages::RayGen | EShaderStages::RayClosestHit | EShaderStages::RayMiss ));

	FG_LOGI( "Test_Shader8 - passed" );
}
