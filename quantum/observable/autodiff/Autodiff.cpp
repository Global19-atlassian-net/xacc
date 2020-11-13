
#include "Autodiff.hpp"
#include "xacc_plugin.hpp"
#include "AllGateVisitor.hpp"
#include "xacc.hpp"
#include <numeric>

namespace {
using namespace xacc;
using namespace xacc::quantum;
using namespace autodiff;
MatrixXcdual X_Mat { MatrixXcdual::Zero(2, 2)};      
MatrixXcdual Y_Mat { MatrixXcdual::Zero(2, 2)};       
MatrixXcdual Z_Mat { MatrixXcdual::Zero(2, 2)};  
MatrixXcdual Id_Mat { MatrixXcdual::Identity(2, 2)};  
MatrixXcdual CX_Mat { MatrixXcdual::Zero(4, 4)};  

constexpr std::complex<double> I { 0.0, 1.0 };

MatrixXcdual kroneckerProduct(MatrixXcdual &lhs, MatrixXcdual &rhs) {
  MatrixXcdual result(lhs.rows() * rhs.rows(), lhs.cols() * rhs.cols());
  for (int i = 0; i < lhs.rows(); ++i) {
    for (int j = 0; j < lhs.cols(); ++j) {
      result.block(i * rhs.rows(), j * rhs.cols(), rhs.rows(), rhs.cols()) =
          lhs(i, j) * rhs;
    }
  }
  return result;
}

class AutodiffCircuitVisitor : public AllGateVisitor {
public:
  AutodiffCircuitVisitor(
      size_t in_nbQubits,
      const std::unordered_map<std::string, autodiff::dual> &in_varMap)
      : m_nbQubit(in_nbQubits), m_varMap(in_varMap) {
    static bool static_gate_init = false;
    m_circuitMat =
        MatrixXcdual::Identity(1ULL << in_nbQubits, 1ULL << in_nbQubits);
    if (!static_gate_init) {
      X_Mat << 0.0, 1.0, 1.0, 0.0;
      Y_Mat << 0.0, -I, I, 0.0;
      Z_Mat << 1.0, 0.0, 0.0, -1.0;
      CX_Mat << 1.0, 0.0, 0.0, 0.0,
               0.0, 1.0, 0.0, 0.0,
               0.0, 0.0, 0.0, 1.0,
               0.0, 0.0, 1.0, 0.0;
      static_gate_init = true;
    }
  }
  // Gate visitor Impl
  void visit(Hadamard &h) override {}
  
  void visit(CNOT &cnot) override {
    MatrixXcdual fullMat =
        twoQubitGateExpand(CX_Mat, cnot.bits()[0], cnot.bits()[1]);
    m_circuitMat = fullMat * m_circuitMat;
  }

  void visit(Rz &rz) override {}
  void visit(Ry &ry) override {
    const bool isVariationalGate = ry.getParameter(0).isVariable();
    // IMPORTANT: we can only handle direct variable usage here,
    // i.e. we don't allow expressions of variables here.
    if (isVariationalGate) {
      InstructionParameter p = ry.getParameter(0);
      const std::string varName = p.toString();
      const auto iter = m_varMap.find(varName);
      if (iter != m_varMap.end()) {
        autodiff::dual var = iter->second;
        std::cout << varName << " = " << var << "\n";
        MatrixXcdual ryMat = cos(var / 2) * MatrixXcdual::Identity(2, 2) -
                             I * sin(var / 2) * Y_Mat;
        // std::cout << "Mat:\n" << ryMat << "\n";
        const size_t bitLoc = ry.bits()[0];
        MatrixXcdual fullMat = singleQubitGateExpand(ryMat, bitLoc);
        m_circuitMat = fullMat * m_circuitMat;
      }
      else {
        xacc::error("Unknown variable named '" + varName + "' encountered.");
      }
    }
  }
  void visit(Rx &rx) override {}
  void visit(X &x) override {
    const size_t bitLoc = x.bits()[0];
    MatrixXcdual fullMat = singleQubitGateExpand(X_Mat, bitLoc);
    m_circuitMat = fullMat * m_circuitMat;
  }
  void visit(Y &y) override {}
  void visit(Z &z) override {}
  void visit(CY &cy) override {}
  void visit(CZ &cz) override {}
  void visit(Swap &s) override {}
  void visit(CRZ &crz) override {}
  void visit(CH &ch) override {}
  void visit(S &s) override {}
  void visit(Sdg &sdg) override {}
  void visit(T &t) override {}
  void visit(Tdg &tdg) override {}
  void visit(CPhase &cphase) override {}
  void visit(Measure &measure) override {}
  void visit(Identity &i) override {}
  void visit(U &u) override {}
  void visit(IfStmt &ifStmt) override {}
  // Identifiable Impl
  const std::string name() const override { return ""; }
  const std::string description() const override { return ""; }
  MatrixXcdual getMat() const { return m_circuitMat; }
  MatrixXcdual singleQubitGateExpand(MatrixXcdual& in_gateMat, size_t in_loc) const {
    if (m_nbQubit == 1) {
      return in_gateMat;
    }
    if (in_loc == 0) {
      auto dim = 1ULL << (m_nbQubit - 1);
      MatrixXcdual rhs = MatrixXcdual::Identity(dim, dim);
      return kroneckerProduct(in_gateMat, rhs);
    } else if (in_loc == m_nbQubit - 1) {
      auto dim = 1ULL << (m_nbQubit - 1);
      MatrixXcdual lhs = MatrixXcdual::Identity(dim, dim);
      return kroneckerProduct(lhs, in_gateMat);
    } else {
      auto leftDim = 1ULL << in_loc;
      auto rightDim = 1ULL << (m_nbQubit - 1 - in_loc);
      MatrixXcdual lhs = MatrixXcdual::Identity(leftDim, leftDim);
      MatrixXcdual rhs = MatrixXcdual::Identity(rightDim, rightDim);
      MatrixXcdual lhsKron = kroneckerProduct(lhs, in_gateMat);
      return kroneckerProduct(lhsKron, rhs);
    }
  }

  MatrixXcdual twoQubitGateExpand(MatrixXcdual &in_gateMat, size_t in_bit1,
                                  size_t in_bit2) const {
    const auto matSize = 1ULL << m_nbQubit;
    MatrixXcdual resultMat = MatrixXcdual::Identity(matSize, matSize);
    // Qubit index list (0->(dim-1))
    std::vector<size_t> index_list(m_nbQubit);
    std::iota(index_list.begin(), index_list.end(), 0);
    const std::vector<size_t> idx{m_nbQubit - in_bit2 - 1,
                                  m_nbQubit - in_bit1 - 1};
    for (size_t k = 0; k < matSize; ++k) {
      std::vector<cxdual> oldColumn(matSize);
      for (size_t i = 0; i < matSize; ++i) {
        oldColumn[i] = resultMat(i, k);
      }

      for (size_t i = 0; i < matSize; ++i) {
        size_t local_i = 0;
        for (size_t l = 0; l < idx.size(); ++l) {
          local_i |= ((i >> idx[l]) & 1) << l;
        }

        cxdual res = 0.0;
        for (size_t j = 0; j < (1ULL << idx.size()); ++j) {
          size_t locIdx = i;
          for (size_t l = 0; l < idx.size(); ++l) {
            if (((j >> l) & 1) != ((i >> idx[l]) & 1)) {
              locIdx ^= (1ULL << idx[l]);
            }
          }

          res += oldColumn[locIdx] * in_gateMat(local_i, j);
        }

        resultMat(i, k) = res;
      }
    }

    return resultMat;
  }

private:
  MatrixXcdual m_circuitMat;
  size_t m_nbQubit;
  std::unordered_map<std::string, autodiff::dual> m_varMap;
};
} // namespace

namespace xacc {
namespace quantum {
void Autodiff::fromObservable(std::shared_ptr<Observable> obs) {
  m_nbQubits = obs->nBits();
  const auto dim = 1ULL << m_nbQubits;
  m_obsMat = MatrixXcdual::Zero(dim, dim);
  for (auto &triplet : obs->to_sparse_matrix()) {
    m_obsMat(triplet.row(), triplet.row()) = triplet.coeff();
  }
}

Differentiable::Result
Autodiff::derivative(std::shared_ptr<CompositeInstruction> CompositeInstruction,
                     const std::vector<double> &x) {
  std::cout << "Observable: \n" << m_obsMat << "\n";
  std::cout << "Circuit: \n" << CompositeInstruction->toString() << "\n";
  std::cout << "Number of arguments = " << CompositeInstruction->nVariables()
            << "\n";
  
  // TODO: Handle generic case (more than one variable)
  auto f = [&](autodiff::dual xDual) -> cxdual {
    std::unordered_map<std::string, autodiff::dual> varMap;
    for (const auto &varName : CompositeInstruction->getVariables()) {
      varMap.emplace(varName, xDual);
    }    
    AutodiffCircuitVisitor visitor(m_nbQubits, varMap);
    InstructionIterator iter(CompositeInstruction);
    
    while (iter.hasNext()) {
      auto inst = iter.next();
      if (!inst->isComposite() && inst->isEnabled()) {
        inst->accept(&visitor);
      }
    }
    MatrixXcdual circMat = visitor.getMat();
    VectorXcdual finalState = circMat.col(0);
    VectorXcdual ket = m_obsMat * finalState;
    VectorXcdual bra = VectorXcdual::Zero(finalState.size());
    for (int i = 0; i < finalState.size(); ++i) {
      bra[i] = conj(finalState[i]);
    }
    // std::cout << "Mat:\n" << circMat << "\n";
    // std::cout << "Final state:\n" << finalState << "\n";
    cxdual exp_val = bra.cwiseProduct(ket).sum();
    return exp_val;
  };

  autodiff::dual argVar = x[0];
  auto test = f(argVar);
  std::cout << "Exp-val: " << test << "\n";
  cxdual dudx = autodiff::derivative(f, autodiff::wrt(argVar), autodiff::forward::at(argVar));  
  std::cout << "dudx: " << dudx << "\n";
  // TODO
  return std::make_pair(test.val.real(), std::vector<double> { dudx.val.real() });
}
} // namespace quantum
} // namespace xacc

REGISTER_PLUGIN(xacc::quantum::Autodiff, xacc::Differentiable)