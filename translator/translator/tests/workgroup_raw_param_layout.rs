// A threadgroup argument with AIR struct layout metadata, reached through a non-inlined helper whose access sits
// in a branch: the emitter gives the parameter a raw word view (uint array). The interface pass used to type the
// workgroup variable from the AIR layout instead, so the word-indexed chains became whole-struct pointers and the
// owned module failed its Load/Store contract (Chrome's on-device-model WebGPU kernels, Dawn/tint shape).
use metal2vulkan::passes::Stage;
use metal2vulkan::translate_sanitized_native;
use std::path::PathBuf;

fn tmp() -> PathBuf {
    let d = std::env::temp_dir().join(format!("m2v_wg_raw_param_layout_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&d);
    d
}

const HELPER_BRANCH: &str = r#"target triple = "air64_v28-apple-macosx26.7.0"

%struct.S5 = type { %struct.WG }
%struct.WG = type { [128 x i32] }

define void @k(ptr addrspace(1) noundef %o, i32 noundef %li, ptr addrspace(3) noundef "air-buffer-no-alias" %t) #0 {
entry:
  %m = getelementptr inbounds %struct.S5, ptr addrspace(3) %t, i64 0, i32 0
  tail call fastcc void @h(i32 noundef %li, ptr addrspace(1) noundef %o, ptr addrspace(3) noundef %m)
  ret void
}

define internal fastcc void @h(i32 noundef %li, ptr addrspace(1) noundef %o, ptr addrspace(3) noundef %wg) #1 {
entry:
  %cmp = icmp ult i32 %li, 128
  %idxprom = zext i32 %li to i64
  br i1 %cmp, label %if.then, label %if.end

if.then:
  %arrayidx = getelementptr inbounds %struct.WG, ptr addrspace(3) %wg, i64 0, i32 0, i64 %idxprom
  store i32 0, ptr addrspace(3) %arrayidx, align 4
  br label %if.end

if.end:
  tail call void @air.wg.barrier(i32 2, i32 1)
  %arrayidx3 = getelementptr inbounds %struct.WG, ptr addrspace(3) %wg, i64 0, i32 0, i64 %idxprom
  %0 = load i32, ptr addrspace(3) %arrayidx3, align 4
  %arrayidx5 = getelementptr inbounds i32, ptr addrspace(1) %o, i64 %idxprom
  store i32 %0, ptr addrspace(1) %arrayidx5, align 4
  ret void
}

declare void @air.wg.barrier(i32, i32)

attributes #0 = { convergent nounwind }
attributes #1 = { convergent noinline nounwind }

!air.kernel = !{!14}
!14 = !{ptr @k, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!18 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"li"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.struct_type_info", !20, !"air.arg_type_size", i32 512, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"S5", !"air.arg_name", !"t"}
!20 = !{!"air.struct_type_info", !21, i32 0, i32 512, i32 0, !"WG", !"m"}
!21 = !{i32 0, i32 4, i32 128, !"uint", !"e"}
"#;

#[test]
fn a_layout_typed_threadgroup_argument_keeps_the_raw_word_view_its_helper_uses() {
    let spv = translate_sanitized_native(HELPER_BRANCH, Stage::Kernel, &tmp())
        .expect("the branch-guarded helper access to a layout-typed threadgroup argument translates");
    assert!(!spv.is_empty());
}
