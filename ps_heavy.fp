!!TS1.0
texture_2d();
texture_2d();
texture_2d();
texture_2d();

!!RC1.0
# All eight general combiner stages, four textures, dot products, mux and fog.
const0 = (0.85, 0.70, 0.55, 1.0);
const1 = (0.20, 0.45, 0.90, 0.5);
{
  rgb {
    spare0 = tex0 * col0;
    spare1 = tex1 * const0;
  }
  alpha {
    spare0 = tex0.a * col0.a;
  }
}
{
  rgb {
    spare0 = expand(spare0) . expand(tex2);
    spare1 = spare1 * tex3;
  }
}
{
  rgb {
    discard = spare0 * tex3;
    discard = spare1 * col1;
    spare0 = sum();
  }
}
{
  rgb {
    discard = spare0 * const1;
    discard = unsigned_invert(spare0) * tex2;
    spare1 = sum();
    scale_by_two();
  }
}
{
  rgb {
    spare0 = spare1 * spare0;
    spare1 = tex0 . tex1;
  }
}
{
  rgb {
    discard = spare0 * col0;
    discard = spare1 * tex3;
    spare0 = mux();
  }
}
{
  rgb {
    spare0 = spare0 * const0;
    scale_by_two();
  }
}
{
  rgb {
    spare0 = spare0 * col0;
  }
  alpha {
    spare0 = spare0.a * col0.a;
  }
}
final_product = spare0 * col1;
out.rgb = lerp(fog.a, spare0, fog.rgb) + final_product;
out.a = spare0.a;
