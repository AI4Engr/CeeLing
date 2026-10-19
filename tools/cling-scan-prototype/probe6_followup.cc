#include <Eigen/Dense>
template <class T> struct Show;
void probe(Eigen::MatrixXd& a, double* p, const double* cp) {
  // A. members that need explicit template arguments
  Show<decltype(a.block<2,2>(0,0))>              b1;
  Show<decltype(a.block(0,0,2,2))>               b2;   // the runtime-size overload
  Show<decltype(a.topLeftCorner<2,2>())>         b3;
  Show<decltype(a.topLeftCorner(2,2))>           b4;
  Show<decltype(a.cast<float>())>                b5;
  Show<decltype(a.lpNorm<1>())>                  b6;
  Show<decltype(a.replicate<2,2>())>             b7;
  Show<decltype(a.template triangularView<Eigen::Upper>())>  b8;
  Show<decltype(a.template selfadjointView<Eigen::Upper>())> b9;
  Show<decltype(a.template conjugateIf<false>())>            b10;
  Show<decltype(a.coeffRef(0,0))>                b11;
  // B. static factories that wrap caller memory (the dangerous kind)
  Show<decltype(Eigen::Map<Eigen::MatrixXd>(p, 2, 2))>        m1;
  Show<decltype(Eigen::MatrixXd::Map(p, 2, 2))>               m2;
  Show<decltype(Eigen::MatrixXd::Map(cp, 2, 2))>              m3;
  // C. resize family with NoChange
  Show<decltype(a.resize(2, Eigen::NoChange))>                r1;
  Show<decltype(a.conservativeResize(2, 2))>                  r2;
  // D. static factories already seen
  Show<decltype(Eigen::MatrixXd::Identity(2,2))>              s1;
  Show<decltype(Eigen::MatrixXd::Zero(2,2))>                  s2;
  Show<decltype(Eigen::MatrixXd::UnitX(2))>                   s3;
  Show<decltype(Eigen::MatrixXd::Random(2,2))>                s4;
  Show<decltype(Eigen::MatrixXd::Constant(2,2,1.0))>          s5;
}
